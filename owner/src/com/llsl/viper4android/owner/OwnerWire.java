package com.llsl.viper4android.owner;

import java.nio.ByteBuffer;
import java.nio.ByteOrder;
import java.util.Arrays;

/** Byte-compatible owner endpoint codec. No App or Gradle classes are required. */
final class OwnerWire {
    static final int PROTOCOL_VERSION = 1;
    static final int FRAME_HEADER_SIZE = 36;
    static final int MAX_FRAME_SIZE = 1024 * 1024;
    static final int MAX_PAYLOAD_SIZE = MAX_FRAME_SIZE - FRAME_HEADER_SIZE;
    static final String SOCKET_NAME = "viper4android.owner.v1";

    static final int OWNER_HELLO = 300;
    static final int OWNER_HELLO_ACK = 301;
    static final int OWN_SESSION = 302;
    static final int OWNED = 303;
    static final int OWN_FAILED = 304;
    static final int RELEASE_SESSION = 305;
    static final int RELEASED = 306;
    static final int SESSION_DELTA = 307;

    static final int HIDL_SELECTOR = 1;
    static final int AIDL_SELECTOR = 2;

    static final int OWNER_HELLO_SIZE = 20;
    static final int OWNER_HELLO_ACK_SIZE = 16;
    static final int OWN_SESSION_SIZE = 16;
    static final int OWNED_SIZE = 16;
    static final int OWNER_FAILED_SIZE = 16;
    static final int RELEASE_SIZE = 12;
    static final int SESSION_DELTA_SIZE = 16;

    private static final byte[] MAGIC = new byte[] {'V', '4', 'A', 'D'};

    static final class Frame {
        final int messageType;
        final long requestId;
        final long sequence;
        final byte[] payload;

        Frame(int messageType, long requestId, long sequence, byte[] payload) {
            this.messageType = messageType;
            this.requestId = requestId;
            this.sequence = sequence;
            this.payload = payload;
        }
    }

    static final class Hello {
        final long ownerPid;
        final long bootId;

        Hello(long ownerPid, long bootId) {
            this.ownerPid = ownerPid;
            this.bootId = bootId;
        }
    }

    static final class HelloAck {
        final boolean accepted;
        final long daemonGeneration;

        HelloAck(boolean accepted, long daemonGeneration) {
            this.accepted = accepted;
            this.daemonGeneration = daemonGeneration;
        }
    }

    static final class OwnSession {
        final int audioSessionId;
        final int selector;

        OwnSession(int audioSessionId, int selector) {
            this.audioSessionId = audioSessionId;
            this.selector = selector;
        }
    }

    static final class Owned {
        final int audioSessionId;
        final int effectId;
        final boolean hasControl;

        Owned(int audioSessionId, int effectId, boolean hasControl) {
            this.audioSessionId = audioSessionId;
            this.effectId = effectId;
            this.hasControl = hasControl;
        }
    }

    static final class OwnerFailed {
        final int audioSessionId;
        final int reasonCode;

        OwnerFailed(int audioSessionId, int reasonCode) {
            this.audioSessionId = audioSessionId;
            this.reasonCode = reasonCode;
        }
    }

    static final class SessionDelta {
        final int audioSessionId;
        final int clientUid;
        final boolean appeared;

        SessionDelta(int audioSessionId, int clientUid, boolean appeared) {
            this.audioSessionId = audioSessionId;
            this.clientUid = clientUid;
            this.appeared = appeared;
        }
    }

    private OwnerWire() {}

    static byte[] frame(int messageType, long requestId, long sequence, byte[] payload) {
        if (payload.length > MAX_PAYLOAD_SIZE) throw new IllegalArgumentException("payload too large");
        ByteBuffer buffer = ByteBuffer.allocate(FRAME_HEADER_SIZE + payload.length)
                .order(ByteOrder.LITTLE_ENDIAN);
        buffer.put(MAGIC);
        buffer.putShort((short) PROTOCOL_VERSION);
        buffer.putShort((short) messageType);
        buffer.putInt(0);
        buffer.putLong(requestId);
        buffer.putLong(sequence);
        buffer.putInt(payload.length);
        buffer.putInt(crc32(payload));
        buffer.put(payload);
        return buffer.array();
    }

    static Frame decodeFrame(byte[] bytes, int length) {
        if (length < FRAME_HEADER_SIZE || length > MAX_FRAME_SIZE) {
            throw new IllegalArgumentException("owner frame size mismatch");
        }
        ByteBuffer buffer = ByteBuffer.wrap(bytes, 0, length).order(ByteOrder.LITTLE_ENDIAN);
        for (byte expected : MAGIC) if (buffer.get() != expected) throw new IllegalArgumentException("bad owner magic");
        int version = Short.toUnsignedInt(buffer.getShort());
        if (version != PROTOCOL_VERSION) throw new IllegalArgumentException("unsupported owner protocol");
        int messageType = Short.toUnsignedInt(buffer.getShort());
        int flags = buffer.getInt();
        if (flags != 0) throw new IllegalArgumentException("unknown owner flags");
        long requestId = buffer.getLong();
        long sequence = buffer.getLong();
        int payloadLength = buffer.getInt();
        int expectedCrc = buffer.getInt();
        if (payloadLength < 0 || payloadLength > MAX_PAYLOAD_SIZE
                || payloadLength != length - FRAME_HEADER_SIZE) {
            throw new IllegalArgumentException("owner frame payload length mismatch");
        }
        byte[] payload = new byte[payloadLength];
        buffer.get(payload);
        if (crc32(payload) != expectedCrc) throw new IllegalArgumentException("owner frame crc mismatch");
        return new Frame(messageType, requestId, sequence, payload);
    }

    static byte[] hello(long ownerPid, long bootId) {
        if (ownerPid == 0 || bootId == 0) throw new IllegalArgumentException("owner identity is zero");
        ByteBuffer b = payload(OWNER_HELLO_SIZE);
        b.putShort((short) PROTOCOL_VERSION).putShort((short) 0).putLong(ownerPid).putLong(bootId);
        return b.array();
    }

    static Hello decodeHello(byte[] bytes) {
        ByteBuffer b = fixed(bytes, OWNER_HELLO_SIZE);
        requireVersionAndReserved(b);
        long pid = b.getLong();
        long boot = b.getLong();
        if (pid == 0 || boot == 0) throw new IllegalArgumentException("owner identity is zero");
        return new Hello(pid, boot);
    }

    static byte[] helloAck(boolean accepted, long daemonGeneration) {
        if (daemonGeneration == 0) throw new IllegalArgumentException("daemon generation is zero");
        ByteBuffer b = payload(OWNER_HELLO_ACK_SIZE);
        b.putShort((short) PROTOCOL_VERSION).putShort((short) (accepted ? 1 : 0));
        b.putLong(daemonGeneration).putInt(0);
        return b.array();
    }

    static HelloAck decodeHelloAck(byte[] bytes) {
        ByteBuffer b = fixed(bytes, OWNER_HELLO_ACK_SIZE);
        requireVersion(b);
        int accepted = Short.toUnsignedInt(b.getShort());
        if (accepted > 1) throw new IllegalArgumentException("invalid owner acceptance");
        long generation = b.getLong();
        if (b.getInt() != 0 || generation == 0) throw new IllegalArgumentException("invalid owner ack");
        return new HelloAck(accepted != 0, generation);
    }

    static byte[] ownSession(int audioSessionId, int selector) {
        if (audioSessionId != 0) throw new IllegalArgumentException("only session zero is supported");
        requireSelector(selector);
        ByteBuffer b = payload(OWN_SESSION_SIZE);
        b.putShort((short) PROTOCOL_VERSION).putShort((short) selector).putInt(audioSessionId);
        b.putInt(0).putInt(0);
        return b.array();
    }

    static OwnSession decodeOwnSession(byte[] bytes) {
        ByteBuffer b = fixed(bytes, OWN_SESSION_SIZE);
        requireVersion(b);
        int selector = Short.toUnsignedInt(b.getShort());
        requireSelector(selector);
        int session = b.getInt();
        if (b.getInt() != 0 || b.getInt() != 0 || session != 0) {
            throw new IllegalArgumentException("invalid owner session request");
        }
        return new OwnSession(session, selector);
    }

    static byte[] owned(int audioSessionId, int effectId, boolean hasControl) {
        if (audioSessionId != 0 || effectId == 0) throw new IllegalArgumentException("invalid owned effect");
        ByteBuffer b = payload(OWNED_SIZE);
        b.putShort((short) PROTOCOL_VERSION).putShort((short) (hasControl ? 1 : 0));
        b.putInt(audioSessionId).putInt(effectId).putInt(0);
        return b.array();
    }

    static Owned decodeOwned(byte[] bytes) {
        ByteBuffer b = fixed(bytes, OWNED_SIZE);
        requireVersion(b);
        int control = Short.toUnsignedInt(b.getShort());
        if (control > 1) throw new IllegalArgumentException("invalid owner control flag");
        int session = b.getInt();
        int effect = b.getInt();
        if (b.getInt() != 0 || session != 0 || effect == 0) throw new IllegalArgumentException("invalid owned effect");
        return new Owned(session, effect, control != 0);
    }

    static byte[] ownerFailed(int audioSessionId, int reasonCode) {
        if (reasonCode == 0) throw new IllegalArgumentException("owner failure reason is zero");
        ByteBuffer b = payload(OWNER_FAILED_SIZE);
        b.putShort((short) PROTOCOL_VERSION).putShort((short) 0);
        b.putInt(audioSessionId).putInt(reasonCode).putInt(0);
        return b.array();
    }

    static byte[] release() {
        ByteBuffer b = payload(RELEASE_SIZE);
        b.putShort((short) PROTOCOL_VERSION).putShort((short) 0).putInt(0).putInt(0);
        return b.array();
    }

    static byte[] released() {
        return release();
    }

    static byte[] sessionDelta(int sessionId, int clientUid, boolean appeared) {
        if (sessionId == 0) throw new IllegalArgumentException("session id is zero");
        ByteBuffer b = payload(SESSION_DELTA_SIZE);
        b.putShort((short) PROTOCOL_VERSION).putShort((short) (appeared ? 1 : 0));
        b.putInt(sessionId).putInt(clientUid).putInt(0);
        return b.array();
    }

    static SessionDelta decodeSessionDelta(byte[] bytes) {
        ByteBuffer b = fixed(bytes, SESSION_DELTA_SIZE);
        requireVersion(b);
        int appeared = Short.toUnsignedInt(b.getShort());
        int session = b.getInt();
        int uid = b.getInt();
        if (appeared > 1 || b.getInt() != 0 || session == 0) throw new IllegalArgumentException("invalid session delta");
        return new SessionDelta(session, uid, appeared != 0);
    }

    private static ByteBuffer payload(int size) {
        return ByteBuffer.allocate(size).order(ByteOrder.LITTLE_ENDIAN);
    }

    static int decodeRelease(byte[] bytes) {
        ByteBuffer b = fixed(bytes, RELEASE_SIZE);
        requireVersionAndReserved(b);
        int session = b.getInt();
        if (b.getInt() != 0 || session != 0) throw new IllegalArgumentException("invalid release session");
        return session;
    }

    private static ByteBuffer fixed(byte[] bytes, int expected) {
        if (bytes.length != expected) throw new IllegalArgumentException("owner payload size mismatch");
        return ByteBuffer.wrap(bytes).order(ByteOrder.LITTLE_ENDIAN);
    }

    private static void requireVersion(ByteBuffer b) {
        if (Short.toUnsignedInt(b.getShort()) != PROTOCOL_VERSION) throw new IllegalArgumentException("unsupported owner version");
    }

    private static void requireVersionAndReserved(ByteBuffer b) {
        requireVersion(b);
        if (b.getShort() != 0) throw new IllegalArgumentException("owner reserved field is non-zero");
    }

    private static void requireSelector(int selector) {
        if (selector != HIDL_SELECTOR && selector != AIDL_SELECTOR) {
            throw new IllegalArgumentException("unsupported effect selector");
        }
    }

    static int crc32(byte[] bytes) {
        int crc = 0xFFFFFFFF;
        for (byte value : bytes) {
            crc ^= value & 0xFF;
            for (int bit = 0; bit < 8; bit++) {
                int mask = -(crc & 1);
                crc = (crc >>> 1) ^ (0xEDB88320 & mask);
            }
        }
        return ~crc;
    }
}
