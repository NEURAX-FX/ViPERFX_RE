package com.llsl.viper4android.probe;

import android.media.AudioAttributes;
import android.media.AudioFormat;
import android.media.AudioTrack;
import android.media.audiofx.AudioEffect;
import java.lang.reflect.Constructor;
import java.lang.reflect.Method;
import java.nio.ByteBuffer;
import java.nio.ByteOrder;
import java.util.UUID;

/**
 * Asserts the ViPER driver reports streaming while no App is running.
 *
 * The driver's PARAM_GET_STREAMING is edge-based: it compares the engine's
 * processed-frame counter against the previous read, so it only answers 1 when
 * audio is actually moving through the graph. Reading it therefore requires both
 * a live playback stream and an effect client to ask through, which is why this
 * probe generates its own tone instead of relying on a player binary the device
 * does not ship.
 *
 * The probe attaches its own session-0 handle purely as a measurement instrument.
 * That is deliberate and temporary: it is released before exit, and module-count
 * exclusivity is asserted by the other acceptance scripts, not this one.
 */
public final class StreamingProbe {
    private static final UUID HIDL_TYPE_UUID = UUID.fromString("ec7178ec-e5e1-4432-a3f4-4657e6795210");
    private static final UUID EFFECT_UUID = UUID.fromString("90380da3-8536-4744-a6a3-5731970e640f");

    private static final int PARAM_GET_STREAMING = 3;
    private static final int SESSION_ID = 0;
    private static final int SAMPLE_RATE = 48000;
    private static final int TONE_HZ = 440;
    // Long enough for the mixer to open an output and the graph to process a few
    // buffers; short enough that a wedged probe fails the script quickly.
    private static final int POLL_ATTEMPTS = 60;
    private static final long POLL_INTERVAL_MS = 100L;
    // Keeps the tone playing after streaming is confirmed, so a caller sampling the
    // daemon's tracked_sessions has a live playback session to observe. Without it
    // the probe can exit in well under a second and the session is gone before the
    // sample.
    private static final long HOLD_AFTER_CONFIRM_MS = 6000L;

    private StreamingProbe() {}

    public static void main(String[] args) {
        AudioEffect effect = null;
        AudioTrack track = null;
        Thread writer = null;
        int streaming = -1;
        try {
            Constructor<AudioEffect> constructor = AudioEffect.class.getConstructor(
                    UUID.class, UUID.class, int.class, int.class);
            constructor.setAccessible(true);
            effect = constructor.newInstance(HIDL_TYPE_UUID, EFFECT_UUID, 0, SESSION_ID);
            effect.setEnabled(true);
            System.out.println("probe_effect_id=" + effect.getId());
            System.out.println("probe_has_control=" + (effect.hasControl() ? 1 : 0));

            track = buildTrack();
            track.play();
            writer = startTone(track);

            Method getParameter = AudioEffect.class.getMethod(
                    "getParameter", byte[].class, byte[].class);
            for (int attempt = 0; attempt < POLL_ATTEMPTS; attempt++) {
                Thread.sleep(POLL_INTERVAL_MS);
                int value = readInt(getParameter, effect, PARAM_GET_STREAMING);
                if (value == 1) {
                    streaming = 1;
                    break;
                }
                streaming = value;
            }
            // Announce before holding, so a caller can react to the result while the
            // stream is still live rather than waiting for the process to exit.
            System.out.println("streaming_confirmed=" + streaming);
            if (streaming == 1) Thread.sleep(HOLD_AFTER_CONFIRM_MS);
        } catch (Throwable failure) {
            Throwable cause = failure;
            while (cause instanceof java.lang.reflect.InvocationTargetException
                && cause.getCause() != null) {
                cause = cause.getCause();
            }
            System.err.println("probe failed: " + cause);
            cause.printStackTrace();
            System.out.println("streaming=-1");
            System.exit(1);
        } finally {
            if (writer != null) writer.interrupt();
            if (track != null) {
                try { track.stop(); } catch (Throwable ignored) {}
                track.release();
            }
            if (effect != null) {
                try { effect.setEnabled(false); } catch (Throwable ignored) {}
                effect.release();
            }
        }
        System.out.println("streaming=" + streaming);
        System.exit(streaming == 1 ? 0 : 2);
    }

    private static AudioTrack buildTrack() {
        AudioAttributes attributes = new AudioAttributes.Builder()
                .setUsage(AudioAttributes.USAGE_MEDIA)
                .setContentType(AudioAttributes.CONTENT_TYPE_MUSIC)
                .build();
        AudioFormat format = new AudioFormat.Builder()
                .setEncoding(AudioFormat.ENCODING_PCM_16BIT)
                .setSampleRate(SAMPLE_RATE)
                .setChannelMask(AudioFormat.CHANNEL_OUT_STEREO)
                .build();
        int minBytes = AudioTrack.getMinBufferSize(
                SAMPLE_RATE, AudioFormat.CHANNEL_OUT_STEREO, AudioFormat.ENCODING_PCM_16BIT);
        // A too-small buffer underruns immediately, which stalls the frame counter
        // and makes a working driver look idle.
        int bufferBytes = Math.max(minBytes, SAMPLE_RATE / 4 * 4);
        return new AudioTrack.Builder()
                .setAudioAttributes(attributes)
                .setAudioFormat(format)
                .setBufferSizeInBytes(bufferBytes)
                .setTransferMode(AudioTrack.MODE_STREAM)
                .build();
    }

    /** Writes a continuous sine tone until interrupted. */
    private static Thread startTone(AudioTrack track) {
        final int frames = SAMPLE_RATE / 10;
        final byte[] buffer = new byte[frames * 4];
        ByteBuffer writer = ByteBuffer.wrap(buffer).order(ByteOrder.LITTLE_ENDIAN);
        for (int frame = 0; frame < frames; frame++) {
            double phase = 2.0 * Math.PI * TONE_HZ * frame / SAMPLE_RATE;
            // Well below full scale: this runs on a real device's speaker.
            short sample = (short) (Math.sin(phase) * 6000.0);
            writer.putShort(sample);
            writer.putShort(sample);
        }
        Thread thread = new Thread(() -> {
            while (!Thread.currentThread().isInterrupted()) {
                int written = track.write(buffer, 0, buffer.length);
                if (written < 0) return;
            }
        }, "viper-probe-tone");
        thread.setDaemon(true);
        thread.start();
        return thread;
    }

    private static int readInt(Method getParameter, AudioEffect effect, int param)
            throws Exception {
        byte[] paramBytes = ByteBuffer.allocate(4)
                .order(ByteOrder.LITTLE_ENDIAN).putInt(param).array();
        byte[] valueBytes = new byte[4];
        int status = (Integer) getParameter.invoke(effect, paramBytes, valueBytes);
        if (status < 0) return -1;
        return ByteBuffer.wrap(valueBytes).order(ByteOrder.LITTLE_ENDIAN).getInt();
    }
}
