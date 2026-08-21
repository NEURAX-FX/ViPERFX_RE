package com.llsl.viper4android.owner;

import android.media.audiofx.AudioEffect;
import java.lang.reflect.Constructor;
import java.util.UUID;

/** Owns exactly one session-0 ViPER AudioEffect client. */
final class EffectOwner {
    static final UUID HIDL_TYPE_UUID = UUID.fromString("ec7178ec-e5e1-4432-a3f4-4657e6795210");
    static final UUID AIDL_TYPE_UUID = UUID.fromString("7261676f-6d75-7369-6364-28e2fd3ac39e");
    static final UUID EFFECT_UUID = UUID.fromString("90380da3-8536-4744-a6a3-5731970e640f");

    private static final int SESSION_ID = 0;
    private static final Constructor<AudioEffect> CONSTRUCTOR = findConstructor();

    private AudioEffect effect;
    // Remembered so a rebuilt handle uses the selector the daemon asked for; the
    // daemon does not re-issue OWN_SESSION when audioserver restarts.
    private int selector = OwnerWire.HIDL_SELECTOR;

    boolean createAndEnable(int selector) {
        if (effect != null) return effect.getEnabled();
        this.selector = selector;
        try {
            UUID type = selector == OwnerWire.AIDL_SELECTOR ? AIDL_TYPE_UUID : HIDL_TYPE_UUID;
            effect = CONSTRUCTOR.newInstance(type, EFFECT_UUID, 0, SESSION_ID);
            effect.setEnabled(true);
            return true;
        } catch (Throwable failure) {
            release();
            return false;
        }
    }

    /**
     * True when the handle is still backed by a live audioserver.
     *
     * A handle whose audioserver died keeps its cached id and still answers
     * getEnabled() with the last value it saw, so neither is usable as a liveness
     * signal. Commands are: setEnabled() returns ERROR_DEAD_OBJECT on a dead
     * binder, which is distinct from ERROR_INVALID_OPERATION for merely having lost
     * control to another client. Measured on device: id and enabled stay unchanged
     * while setEnabled() goes 0 -> -7.
     */
    boolean isAlive() {
        AudioEffect current = effect;
        if (current == null) return false;
        try {
            return current.setEnabled(true) != AudioEffect.ERROR_DEAD_OBJECT;
        } catch (Throwable failure) {
            // IllegalStateException from an already-released handle counts as dead.
            return false;
        }
    }

    /**
     * Drops a dead handle and creates a replacement.
     *
     * Returns true when a live handle exists afterwards. Called when audioserver has
     * restarted: every effect client reference in the system died with it, and
     * without this the owner would hold a handle that processes nothing while the
     * daemon still reports owner_state=owned.
     */
    boolean recreate() {
        release();
        return createAndEnable(selector);
    }

    int effectId() {
        return effect == null ? 0 : effect.getId();
    }

    boolean hasControl() {
        return effect != null && effect.hasControl();
    }

    void release() {
        AudioEffect current = effect;
        effect = null;
        if (current != null) {
            try {
                current.setEnabled(false);
            } catch (Throwable ignored) {
                // Binder death/release is already terminal for this handle.
            }
            try {
                current.release();
            } catch (Throwable ignored) {
                // Do not let cleanup prevent the owner loop from closing its socket.
            }
        }
    }

    private static Constructor<AudioEffect> findConstructor() {
        try {
            Constructor<AudioEffect> constructor = AudioEffect.class.getConstructor(
                    UUID.class, UUID.class, int.class, int.class);
            constructor.setAccessible(true);
            return constructor;
        } catch (Exception failure) {
            throw new ExceptionInInitializerError(failure);
        }
    }
}
