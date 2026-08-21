package com.llsl.viper4android.owner;

import android.content.Context;
import android.net.LocalSocket;
import android.net.LocalSocketAddress;
import android.os.Handler;
import android.os.HandlerThread;
import android.os.Looper;
import android.os.Process;
import java.io.IOException;
import java.io.InputStream;
import java.io.OutputStream;
import java.lang.reflect.Method;
import java.net.SocketTimeoutException;
import java.security.SecureRandom;
import java.util.concurrent.atomic.AtomicBoolean;

/** Standalone app_process64 entry point for the init-supervised effect owner. */
public final class OwnerMain {
    private static final int OWNER_UID = 0;
    private static final int REASON_BAD_COMMAND = 1;
    private static final int REASON_CREATE_FAILED = 2;
    // Reported when audioserver restarted and the replacement handle could not be
    // created yet, so the daemon stops publishing a handle that does not exist.
    private static final int REASON_HANDLE_DIED = 3;

    // Poll interval while waiting for a restarted daemon to bind its socket.
    private static final long RECONNECT_POLL_MS = 500L;
    // How often the held handle is checked against a live audioserver. Doubles as
    // the socket read timeout, so it also bounds how long a stale effect id stays
    // published after an audioserver restart.
    private static final long HANDLE_CHECK_INTERVAL_MS = 1000L;
    // How long the owner keeps its effect alive without a daemon. Long enough to
    // cover an init respawn or a module update, short enough that an uninstalled
    // module does not leave a root process holding an effect forever.
    private static final long RECONNECT_WINDOW_NANOS = 120L * 1000L * 1000L * 1000L;

    private final AtomicBoolean stopping = new AtomicBoolean(false);
    private LocalSocket socket;
    private OutputStream output;
    private long sequence = 1;
    private EffectOwner effectOwner;
    private SessionObserver observer;
    private HandlerThread callbackThread;

    public static void main(String[] args) {
        try {
            // The daemon accepts --owner-socket, so the owner must accept the matching
            // name. Hardcoding it would make a daemon on any other socket unreachable.
            String socketName = args.length > 0 && !args[0].isEmpty()
                    ? args[0]
                    : OwnerWire.SOCKET_NAME;
            new OwnerMain().run(socketName);
        } catch (Throwable failure) {
            // Unwrap reflective wrappers: systemContext() goes through reflection, so
            // the outer InvocationTargetException carries no diagnosis at all and the
            // supervisor would only see "owner died".
            Throwable cause = failure;
            while (cause instanceof java.lang.reflect.InvocationTargetException
                && cause.getCause() != null) {
                cause = cause.getCause();
            }
            System.err.println("viper-owner failed: " + cause);
            cause.printStackTrace();
            System.exit(1);
        }
    }

    private void run(String socketName) throws Exception {
        if (Process.myUid() != OWNER_UID) throw new SecurityException("owner must run as root");
        // ActivityThread.systemMain() builds a Handler, which requires a prepared
        // main Looper. Without this the owner dies before it ever connects, and the
        // supervisor can only report "owner died".
        //
        // The loop is never run: this thread blocks on the owner socket instead, and
        // playback callbacks are delivered on their own HandlerThread below.
        if (Looper.getMainLooper() == null) Looper.prepareMainLooper();
        Context context = systemContext();
        if (context.checkSelfPermission("android.permission.MODIFY_AUDIO_ROUTING") != 0) {
            throw new SecurityException("MODIFY_AUDIO_ROUTING is required");
        }

        callbackThread = new HandlerThread("viper-owner-sessions");
        callbackThread.start();
        Handler callbackHandler = new Handler(callbackThread.getLooper());
        observer = new SessionObserver(context, callbackHandler);
        installShutdownHook();
        startHandleWatchdog();

        // The daemon is the control plane, not the effect client. Losing its socket
        // must NOT release the handle: the daemon is restarted by init on every
        // update and crash, and releasing here would silence audio each time, which
        // is exactly what this process exists to prevent. So the link is
        // reconnected while the effect stays held.
        byte[] frameBuffer = new byte[OwnerWire.MAX_FRAME_SIZE];
        boolean everConnected = false;
        long retryDeadlineNanos = 0;
        while (!stopping.get()) {
            if (!openLink(socketName)) {
                // The first connect is the supervisor's handshake. Failing it must be
                // loud, otherwise a misconfigured socket name looks like a hung owner.
                if (!everConnected) throw new IOException("owner socket unreachable: " + socketName);
                // Bounded: an uninstalled module never comes back, and a root process
                // holding an effect forever would be unkillable by design.
                if (System.nanoTime() > retryDeadlineNanos) break;
                sleepQuietly(RECONNECT_POLL_MS);
                continue;
            }
            everConnected = true;
            serve(frameBuffer);
            closeLink();
            retryDeadlineNanos = System.nanoTime() + RECONNECT_WINDOW_NANOS;
        }
        cleanup();
    }

    /** Connects and announces the owner. False means the daemon is not listening. */
    private boolean openLink(String socketName) {
        LocalSocket candidate = new LocalSocket(LocalSocket.SOCKET_SEQPACKET);
        try {
            candidate.connect(new LocalSocketAddress(
                    socketName, LocalSocketAddress.Namespace.ABSTRACT));
            socket = candidate;
            output = candidate.getOutputStream();
            send(OwnerWire.OWNER_HELLO, 0, OwnerWire.hello(Process.myPid(), bootId()));
            return true;
        } catch (IOException failure) {
            try { candidate.close(); } catch (IOException ignored) {}
            socket = null;
            output = null;
            return false;
        }
    }

    /**
     * Serves frames until the daemon's socket ends. The effect is left untouched.
     *
     * Deliberately a plain blocking read with no socket timeout. `LocalSocket`'s
     * `setSoTimeout()` does not raise `SocketTimeoutException` here: the timeout
     * arrives as a generic `IOException`, indistinguishable from a dropped link, so
     * using it made every tick look like a daemon disconnect (measured: ~1 bogus
     * `owner_restarts` per second with audioserver perfectly healthy). Handle
     * liveness is polled on its own thread instead.
     */
    private void serve(byte[] frameBuffer) throws IOException {
        InputStream input = socket.getInputStream();
        while (!stopping.get()) {
            int length;
            try {
                length = input.read(frameBuffer);
            } catch (IOException dropped) {
                // A daemon killed mid-frame surfaces as a reset, not a clean EOF.
                return;
            }
            if (length < 0) return;
            if (length == 0) continue;
            handle(OwnerWire.decodeFrame(frameBuffer, length));
        }
    }

    /**
     * Polls handle liveness off the socket thread.
     *
     * A separate thread rather than a read timeout: the main loop must stay blocked
     * on the socket, and `LocalSocket.setSoTimeout()` cannot be distinguished from a
     * dropped link (see serve()). Daemon-independent by design, because audioserver
     * can die while no daemon is connected.
     */
    private void startHandleWatchdog() {
        Thread watchdog = new Thread(() -> {
            while (!stopping.get()) {
                sleepQuietly(HANDLE_CHECK_INTERVAL_MS);
                if (stopping.get()) return;
                try {
                    checkHandleLiveness();
                } catch (Throwable failure) {
                    // Never let a probe failure kill the thread that repairs the handle.
                    System.err.println("viper-owner handle check failed: " + failure);
                }
            }
        }, "viper-owner-handle");
        watchdog.setDaemon(true);
        watchdog.start();
    }

    /**
     * Rebuilds the effect when audioserver has restarted under it.
     *
     * Every AudioEffect client reference dies with audioserver, but the handle object
     * survives and keeps reporting its old id and enabled flag; only commands fail,
     * with ERROR_DEAD_OBJECT. Without this the owner holds a handle that processes
     * nothing while the daemon still publishes owner_state=owned, so the App stays
     * out of the way and audio is silently unprocessed.
     *
     * Synchronized against handle(): an OWN_SESSION arriving on the socket thread
     * must not race a rebuild here over the same EffectOwner.
     */
    private synchronized void checkHandleLiveness() {
        EffectOwner current = effectOwner;
        if (current == null || current.isAlive()) return;

        if (!current.recreate()) {
            // Audioserver may still be coming back up. Report it so the daemon stops
            // claiming a handle that does not exist, and let the next tick retry.
            trySend(OwnerWire.OWN_FAILED, 0, OwnerWire.ownerFailed(0, REASON_HANDLE_DIED));
            return;
        }
        // A rebuilt handle has a new effect id, so the daemon's published id would
        // otherwise stay stale.
        trySend(
                OwnerWire.OWNED,
                0,
                OwnerWire.owned(0, current.effectId(), current.hasControl()));
    }

    /** Sends without letting a dropped link kill the owner; the effect matters more. */
    private void trySend(int messageType, long requestId, byte[] payload) {
        try {
            send(messageType, requestId, payload);
        } catch (IOException ignored) {
            // The daemon went away mid-report. The main loop reconnects and the
            // supervisor re-commands ownership.
        }
    }

    private void closeLink() {
        LocalSocket current = socket;
        socket = null;
        output = null;
        if (current != null) {
            try { current.close(); } catch (IOException ignored) {}
        }
    }

    private static void sleepQuietly(long millis) {
        try {
            Thread.sleep(millis);
        } catch (InterruptedException interrupted) {
            Thread.currentThread().interrupt();
        }
    }

    // Synchronized against the handle watchdog: OWN_SESSION/RELEASE_SESSION and a
    // liveness rebuild both mutate effectOwner.
    private synchronized void handle(OwnerWire.Frame frame) throws IOException {
        switch (frame.messageType) {
            case OwnerWire.OWNER_HELLO_ACK:
                OwnerWire.HelloAck ack = OwnerWire.decodeHelloAck(frame.payload);
                if (!ack.accepted) throw new SecurityException("owner hello refused");
                break;
            case OwnerWire.OWN_SESSION:
                OwnerWire.OwnSession request = OwnerWire.decodeOwnSession(frame.payload);
                // A restarted daemon re-issues OWN_SESSION on the new connection.
                // Releasing and recreating here would hand back a different effect id
                // and make AudioFlinger see the module disappear, so a handle that is
                // already held is reported as-is.
                if (effectOwner == null || effectOwner.effectId() == 0) {
                    if (effectOwner != null) effectOwner.release();
                    effectOwner = new EffectOwner();
                    if (!effectOwner.createAndEnable(request.selector)) {
                        send(OwnerWire.OWN_FAILED, frame.requestId,
                                OwnerWire.ownerFailed(request.audioSessionId, REASON_CREATE_FAILED));
                        break;
                    }
                }
                send(OwnerWire.OWNED, frame.requestId,
                        OwnerWire.owned(request.audioSessionId, effectOwner.effectId(), effectOwner.hasControl()));
                if (observer != null) {
                    observer.start(delta -> {
                        try {
                            send(OwnerWire.SESSION_DELTA, 0,
                                    OwnerWire.sessionDelta(delta.audioSessionId, delta.clientUid, delta.appeared));
                        } catch (IOException failure) {
                            // The daemon went away. Drop only the link: the effect stays
                            // held and the main loop reconnects.
                            closeLink();
                        }
                    });
                }
                break;
            case OwnerWire.RELEASE_SESSION:
                OwnerWire.decodeRelease(frame.payload);
                release(frame.requestId);
                break;
            default:
                send(OwnerWire.OWN_FAILED, frame.requestId,
                        OwnerWire.ownerFailed(0, REASON_BAD_COMMAND));
                break;
        }
    }

    /**
     * Handles an explicit RELEASE_SESSION.
     *
     * Drops the effect and the observer but keeps the callback thread, because the
     * daemon may ask for the session again on this same connection; quitting the
     * looper would make that second OWN_SESSION unable to deliver session deltas.
     */
    private void release(long requestId) throws IOException {
        if (observer != null) observer.stop();
        if (effectOwner != null) effectOwner.release();
        effectOwner = null;
        send(OwnerWire.RELEASED, requestId, OwnerWire.released());
    }

    /** Terminal teardown: only on process exit, never on a daemon disconnect. */
    private void cleanup() {
        if (observer != null) observer.stop();
        if (effectOwner != null) effectOwner.release();
        if (callbackThread != null) callbackThread.quitSafely();
        observer = null;
        effectOwner = null;
        callbackThread = null;
    }

    private synchronized void send(int messageType, long requestId, byte[] payload) throws IOException {
        OutputStream sink = output;
        // The link can be dropped between a session-delta callback deciding to send
        // and this write. That is a lost diagnostic, not a reason to die.
        if (sink == null) throw new IOException("owner link is closed");
        byte[] frame = OwnerWire.frame(messageType, requestId, sequence++, payload);
        sink.write(frame);
        sink.flush();
    }

    private void installShutdownHook() {
        Runtime.getRuntime().addShutdownHook(new Thread(() -> {
            stopping.set(true);
            cleanup();
            if (socket != null) {
                try { socket.close(); } catch (IOException ignored) {}
            }
        }, "viper-owner-shutdown"));
    }

    private static Context systemContext() throws Exception {
        Class<?> activityThread = Class.forName("android.app.ActivityThread");
        Method systemMain = activityThread.getMethod("systemMain");
        Object thread = systemMain.invoke(null);
        if (thread == null) throw new IllegalStateException("system ActivityThread unavailable");
        Method getSystemContext = activityThread.getMethod("getSystemContext");
        Context context = (Context) getSystemContext.invoke(thread);
        if (context == null) throw new IllegalStateException("system Context unavailable");
        return context;
    }

    private static long bootId() {
        try {
            byte[] random = new byte[8];
            new SecureRandom().nextBytes(random);
            long value = 0;
            for (byte item : random) value = (value << 8) | (item & 0xFFL);
            return value == 0 ? 1 : value;
        } catch (Throwable ignored) {
            return Math.max(1, System.nanoTime());
        }
    }
}
