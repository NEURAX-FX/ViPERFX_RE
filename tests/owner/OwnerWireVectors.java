package com.llsl.viper4android.owner;

/**
 * Emits the same framed owner messages as tests/OwnerWireVectors.cpp, using the
 * production {@link OwnerWire} the ART owner runs on the device.
 *
 * Compiled against a plain host JDK: OwnerWire deliberately depends on nothing
 * but java.nio, so the wire contract can be checked without a device or an
 * android.jar. Keep the message set and constants identical to the native
 * emitter; a diff between the two outputs is a real protocol break.
 */
public final class OwnerWireVectors {
    private static final long REQUEST_ID = 0x1122334455667788L;
    private static final long SEQUENCE = 0x0807060504030201L;

    private OwnerWireVectors() {}

    private static void emit(String name, int messageType, byte[] payload) {
        byte[] frame = OwnerWire.frame(messageType, REQUEST_ID, SEQUENCE, payload);
        StringBuilder hex = new StringBuilder(frame.length * 2);
        for (byte value : frame) hex.append(String.format("%02x", value));
        System.out.println(name + " " + hex);
    }

    public static void main(String[] args) {
        emit("owner_hello", OwnerWire.OWNER_HELLO,
                OwnerWire.hello(0x11223344L, 0x0102030405060708L));
        emit("owner_hello_ack", OwnerWire.OWNER_HELLO_ACK, OwnerWire.helloAck(true, 7L));
        emit("own_session_hidl", OwnerWire.OWN_SESSION,
                OwnerWire.ownSession(0, OwnerWire.HIDL_SELECTOR));
        emit("own_session_aidl", OwnerWire.OWN_SESSION,
                OwnerWire.ownSession(0, OwnerWire.AIDL_SELECTOR));
        emit("owned", OwnerWire.OWNED, OwnerWire.owned(0, 66139, true));
        emit("own_failed", OwnerWire.OWN_FAILED, OwnerWire.ownerFailed(0, 5));
        emit("release_session", OwnerWire.RELEASE_SESSION, OwnerWire.release());
        emit("released", OwnerWire.RELEASED, OwnerWire.released());
        emit("session_delta_appeared", OwnerWire.SESSION_DELTA,
                OwnerWire.sessionDelta(42, 10123, true));
        emit("session_delta_gone", OwnerWire.SESSION_DELTA,
                OwnerWire.sessionDelta(42, 10123, false));
    }
}
