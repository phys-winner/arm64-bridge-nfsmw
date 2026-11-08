package com.ea.ironmonkey.port;

import android.media.AudioAttributes;
import android.media.AudioFormat;
import android.media.AudioManager;
import android.media.AudioTrack;
import android.os.Build;

/**
 * FMOD Ex output track (modernization 5.5, lessons §5.2). org.fmod.FMODAudioDevice created its AudioTrack with the
 * legacy STREAM_MUSIC constructor, which the platform may route to the deep-buffer output. On API 26+ the track is
 * built with USAGE_GAME and PERFORMANCE_MODE_LOW_LATENCY (volume keys still control media volume); the platform
 * may refuse the configuration, so any failure falls back to the original constructor with the same format and
 * buffer size. Buffer size and sample rate stay those chosen by FMOD (never below AudioTrack.getMinBufferSize).
 */
public final class NfsAudio {
    private static final String TAG = "NfsAudio";

    private NfsAudio() {}

    @SuppressWarnings("deprecation")
    public static AudioTrack createTrack(int sampleRate, int bufferBytes) {
        if (Build.VERSION.SDK_INT >= 26) {
            try {
                AudioTrack t = new AudioTrack.Builder()
                        .setAudioAttributes(new AudioAttributes.Builder()
                                .setUsage(AudioAttributes.USAGE_GAME)
                                .setContentType(AudioAttributes.CONTENT_TYPE_SONIFICATION)
                                .build())
                        .setAudioFormat(new AudioFormat.Builder()
                                .setSampleRate(sampleRate)
                                .setChannelMask(AudioFormat.CHANNEL_OUT_STEREO)
                                .setEncoding(AudioFormat.ENCODING_PCM_16BIT)
                                .build())
                        .setBufferSizeInBytes(bufferBytes)
                        .setTransferMode(AudioTrack.MODE_STREAM)
                        .setPerformanceMode(AudioTrack.PERFORMANCE_MODE_LOW_LATENCY)
                        .build();
                if (t.getState() == AudioTrack.STATE_INITIALIZED) {
                    NfsLog.i(TAG, "low-latency track " + sampleRate + " Hz, " + bufferBytes + " B, frames=" + t.getBufferSizeInFrames()
                            + " perfMode=" + t.getPerformanceMode());
                    return t;
                }
                t.release();
            } catch (Throwable e) {
                NfsLog.w(TAG, "low-latency track refused, legacy fallback: " + e);
            }
        }
        return new AudioTrack(AudioManager.STREAM_MUSIC, sampleRate, AudioFormat.CHANNEL_CONFIGURATION_STEREO,
                AudioFormat.ENCODING_PCM_16BIT, bufferBytes, AudioTrack.MODE_STREAM);
    }
}
