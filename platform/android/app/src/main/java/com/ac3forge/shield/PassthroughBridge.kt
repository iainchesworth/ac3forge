package com.ac3forge.shield

import android.media.AudioAttributes
import android.media.AudioFormat
import android.media.AudioTrack
import android.util.Log
import java.nio.ByteBuffer

private const val TAG = "PassthroughBridge"

// Empirically-generous multiples of one burst, not AudioTrack.getMinBufferSize()
// alone: real compressed-format AudioTrack buffers on Android have been
// documented undersized in the wild (Fire TV reports of getNativeFrameCount
// returning as little as 1024 frames for an AC-3 track), and Kodi's own AC-3
// sink applies its own multiplier on top of the OS minimum rather than trust
// it - see docs/platforms/android.md's buffer-sizing section. Start
// conservative; tune against this Shield+receiver pair once audible
// passthrough is confirmed (see build-order notes in the same doc).
private const val AC3_BURST_BYTES = 6144
private const val EAC3_BURST_BYTES = 24576
private const val BUFFER_BURSTS = 8

/**
 * Owns the Java `android.media.AudioTrack` that actually bitstreams IEC 61937
 * to the HDMI/S-PDIF output - the delivery mechanism
 * `ac3::audio::PassthroughSink`'s Android backend
 * (src/audio/src/backend/android/passthrough.cpp) calls into over JNI. See
 * that file's header comment for the exact contract this class implements
 * (method names/signatures are part of the JNI ABI - do not rename/reshape
 * without updating both sides).
 *
 * Opened with [AudioFormat.ENCODING_IEC61937], not `ENCODING_E_AC3`: the
 * native side already hands this class complete, pre-wrapped IEC 61937
 * bursts (`ac3::iec61937::wrap_frame`/`Eac3BurstPacker::push`'s output), and
 * `ENCODING_E_AC3` would tell Android to wrap them AGAIN, corrupting the
 * stream. `carrierRateHz` in every method below is already the physical
 * carrier rate (content rate for AC-3, 4x it for E-AC-3), computed once in
 * C++ (`android_support.hpp`'s `carrier_rate`) - this class never multiplies
 * by 4 itself, matching the native side's header comment.
 *
 * `AudioTrack.isDirectPlaybackSupported(AudioFormat, AudioAttributes)` is
 * deprecated in favor of `AudioManager.getDirectPlaybackSupport()`, which
 * needs API 33+ - this app's minSdk is 26 for AAudio (see
 * app/build.gradle.kts), and this specific Shield is API 30, so the
 * deprecated static method is the only one that exists on the actual
 * target hardware. Suppressed deliberately, not an oversight.
 */
@Suppress("DEPRECATION")
class PassthroughBridge {
    private var audioTrack: AudioTrack? = null

    /**
     * Probes whether the current system audio route accepts this ONE
     * format/rate combination for direct IEC 61937 playback. Called once
     * per format from native (not batched - see passthrough.cpp), so
     * `carrierRateHz` for the eac3=true call is already 4x the AC-3 call's
     * rate for the same content.
     */
    fun isDirectPlaybackSupported(carrierRateHz: Int, eac3: Boolean): Boolean {
        val format = iec61937Format(carrierRateHz)
        val supported = AudioTrack.isDirectPlaybackSupported(format, MOVIE_ATTRIBUTES)
        Log.d(TAG, "isDirectPlaybackSupported(carrier=$carrierRateHz eac3=$eac3) = $supported")
        return supported
    }

    /** Ordinary (non-compressed) direct PCM support, at the content rate. */
    fun isPcmSupported(sampleRateHz: Int): Boolean {
        val format = AudioFormat.Builder()
            .setEncoding(AudioFormat.ENCODING_PCM_16BIT)
            .setSampleRate(sampleRateHz)
            .setChannelMask(AudioFormat.CHANNEL_OUT_STEREO)
            .build()
        return AudioTrack.isDirectPlaybackSupported(format, MOVIE_ATTRIBUTES)
    }

    /** Opens the AudioTrack at the given (already-carrier) rate. */
    fun open(carrierRateHz: Int, eac3: Boolean): Boolean {
        close()

        val format = iec61937Format(carrierRateHz)
        if (!AudioTrack.isDirectPlaybackSupported(format, MOVIE_ATTRIBUTES)) {
            Log.w(TAG, "open: isDirectPlaybackSupported=false for carrier=$carrierRateHz eac3=$eac3")
            return false
        }

        val burstBytes = if (eac3) EAC3_BURST_BYTES else AC3_BURST_BYTES
        val bufferBytes = burstBytes * BUFFER_BURSTS

        return try {
            val track = AudioTrack.Builder()
                .setAudioFormat(format)
                .setAudioAttributes(MOVIE_ATTRIBUTES)
                .setBufferSizeInBytes(bufferBytes)
                .setTransferMode(AudioTrack.MODE_STREAM)
                .build()
            track.play()
            audioTrack = track
            Log.i(TAG, "open: AudioTrack opened, carrier=$carrierRateHz eac3=$eac3 buffer=$bufferBytes")
            true
        } catch (e: Exception) {
            Log.e(TAG, "open: AudioTrack.Builder().build()/play() threw", e)
            false
        }
    }

    /**
     * Writes one burst non-blocking. Returns the byte count AudioTrack
     * actually accepted (matching `AudioTrack.write`'s own return value),
     * or a negative `AudioTrack.ERROR_*` code - the native side treats
     * anything other than exactly `sizeBytes` as a failed submit and counts
     * a negative result as an underrun (see passthrough.cpp's submit()).
     */
    fun submit(buffer: ByteBuffer, sizeBytes: Int): Int {
        val track = audioTrack ?: return AudioTrack.ERROR_INVALID_OPERATION
        buffer.position(0)
        buffer.limit(sizeBytes)
        return track.write(buffer, sizeBytes, AudioTrack.WRITE_NON_BLOCKING)
    }

    fun close() {
        val track = audioTrack ?: return
        audioTrack = null
        try {
            track.stop()
        } catch (e: IllegalStateException) {
            // Not yet playing (open()'s isDirectPlaybackSupported check
            // failed before play() ran) - release() below still applies.
        }
        track.release()
    }

    private fun iec61937Format(carrierRateHz: Int): AudioFormat =
        AudioFormat.Builder()
            .setEncoding(AudioFormat.ENCODING_IEC61937)
            .setSampleRate(carrierRateHz)
            // Stereo, not 5.1/7.1: an IEC 61937 burst physically rides a
            // 2-channel 16-bit S/PDIF-shaped carrier no matter how many
            // channels it decodes to on the far end - confirmed against
            // Kodi's AESinkAUDIOTRACK.cpp, which does not exempt E-AC-3 the
            // way it exempts DTS-HD-MA/TrueHD from this same rule.
            .setChannelMask(AudioFormat.CHANNEL_OUT_STEREO)
            .build()

    companion object {
        private val MOVIE_ATTRIBUTES = AudioAttributes.Builder()
            .setUsage(AudioAttributes.USAGE_MEDIA)
            .setContentType(AudioAttributes.CONTENT_TYPE_MOVIE)
            .build()
    }
}
