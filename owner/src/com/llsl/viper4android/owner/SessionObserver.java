package com.llsl.viper4android.owner;

import android.content.Context;
import android.media.AudioManager;
import android.media.AudioPlaybackConfiguration;
import android.os.Handler;
import java.lang.reflect.Method;
import java.util.HashMap;
import java.util.HashSet;
import java.util.List;
import java.util.Map;
import java.util.Set;
import java.util.function.Consumer;

/** Reports real playback session/client-uid deltas from a privileged system Context. */
final class SessionObserver {
    private final AudioManager audioManager;
    private final Handler callbackHandler;
    private final Method sessionMethod;
    private final Method uidMethod;
    private final Map<String, OwnerWire.SessionDelta> known = new HashMap<>();
    private AudioManager.AudioPlaybackCallback callback;
    private Consumer<OwnerWire.SessionDelta> sink;

    SessionObserver(Context context, Handler callbackHandler) {
        Object service = context.getSystemService(Context.AUDIO_SERVICE);
        if (!(service instanceof AudioManager)) throw new IllegalStateException("AudioManager unavailable");
        this.audioManager = (AudioManager) service;
        this.callbackHandler = callbackHandler;
        try {
            this.sessionMethod = AudioPlaybackConfiguration.class.getMethod("getSessionId");
            this.uidMethod = AudioPlaybackConfiguration.class.getMethod("getClientUid");
        } catch (Exception failure) {
            throw new IllegalStateException("AudioPlaybackConfiguration fields unavailable", failure);
        }
    }

    void start(Consumer<OwnerWire.SessionDelta> sink) {
        if (callback != null) return;
        this.sink = sink;
        callback = new AudioManager.AudioPlaybackCallback() {
            @Override
            public void onPlaybackConfigChanged(List<AudioPlaybackConfiguration> configurations) {
                publishDiff(configurations);
            }
        };
        audioManager.registerAudioPlaybackCallback(callback, callbackHandler);
        publishDiff(audioManager.getActivePlaybackConfigurations());
    }

    void stop() {
        AudioManager.AudioPlaybackCallback current = callback;
        callback = null;
        sink = null;
        known.clear();
        if (current != null) audioManager.unregisterAudioPlaybackCallback(current);
    }

    private void publishDiff(List<AudioPlaybackConfiguration> configurations) {
        Set<String> present = new HashSet<>();
        for (AudioPlaybackConfiguration configuration : configurations) {
            try {
                int session = ((Number) sessionMethod.invoke(configuration)).intValue();
                int uid = ((Number) uidMethod.invoke(configuration)).intValue();
                if (session <= 0) continue;
                String key = session + ":" + uid;
                present.add(key);
                if (!known.containsKey(key)) {
                    OwnerWire.SessionDelta delta = new OwnerWire.SessionDelta(session, uid, true);
                    known.put(key, delta);
                    Consumer<OwnerWire.SessionDelta> current = sink;
                    if (current != null) current.accept(delta);
                }
            } catch (Throwable ignored) {
                // A vendor-specific playback entry must not kill the observer.
            }
        }
        for (String key : new HashSet<>(known.keySet())) {
            if (!present.contains(key)) {
                OwnerWire.SessionDelta previous = known.remove(key);
                Consumer<OwnerWire.SessionDelta> current = sink;
                if (current != null) {
                    current.accept(new OwnerWire.SessionDelta(
                            previous.audioSessionId, previous.clientUid, false));
                }
            }
        }
    }
}
