package com.ac3forge.shield

import android.app.Activity
import android.graphics.Color
import android.os.Bundle
import android.util.Log
import android.view.Gravity
import android.view.KeyEvent
import android.view.MotionEvent
import android.widget.FrameLayout
import android.widget.TextView
import kotlin.concurrent.thread

private const val TAG = "ShieldAtmosDemo"

/**
 * Loads the native library, registers the [PassthroughBridge], runs the
 * HDMI capability probe, starts the encode loop (live_cursor.cpp), wires
 * Shield Controller/remote input through [InputController], and shows the
 * live object positions via [RoomView] - the startup/capability report stays
 * as a one-shot logcat entry plus a small on-screen control-hints overlay
 * rather than replacing the room view, since it doesn't change frame to
 * frame the way the room view does.
 *
 * Diagnostic mode: `am start ... --es play_file /sdcard/Download/whatever.ec3`
 * skips the live cursor/object demo entirely and instead streams that real,
 * already-encoded file through file_replay.cpp's PassthroughSink path - see
 * NativeBridge.nativePlayEac3File's doc comment for why this exists.
 */
class MainActivity : Activity() {
    private val passthroughBridge = PassthroughBridge()
    private val inputController = InputController()

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)

        val version = try {
            NativeBridge.nativeVersionString()
        } catch (e: UnsatisfiedLinkError) {
            Log.e(TAG, "native library failed to load/link", e)
            "(native link failed - see logcat)"
        }
        Log.i(TAG, "ac3::forge version reported by native library: $version")

        val capabilities = try {
            NativeBridge.registerPassthroughBridge(passthroughBridge)
            NativeBridge.nativeProbePassthroughCapabilities()
        } catch (e: UnsatisfiedLinkError) {
            Log.e(TAG, "passthrough capability probe failed", e)
            "(capability probe failed - see logcat)"
        }
        Log.i(TAG, "passthrough capability probe:\n$capabilities")

        val playFilePath = intent.getStringExtra("play_file")
        if (playFilePath != null) {
            startFileReplay(playFilePath)
            return
        }

        val liveCursorStarted = try {
            NativeBridge.nativeStartLiveCursor()
        } catch (e: UnsatisfiedLinkError) {
            Log.e(TAG, "encode loop failed to start", e)
            false
        }
        Log.i(TAG, "encode loop started: $liveCursorStarted")

        val title = TextView(this).apply {
            text = "ac3forge — Shield Atmos Demo"
            textSize = 20f
            setTextColor(Color.WHITE)
            setBackgroundColor(Color.argb(180, 0, 0, 0))
            setPadding(24, 12, 24, 12)
        }
        val hints = TextView(this).apply {
            text = "Stick/D-pad: push the lead object off its course, it drifts back when you " +
                "let go   •   Right stick / L1+R1: height   •   Long-press A/center: D-pad " +
                "up/down controls height instead of depth\n" +
                "● lead (yours to push around)   ● ● two ambient tones, always on their own course"
            textSize = 14f
            gravity = Gravity.CENTER
            setTextColor(Color.LTGRAY)
            setBackgroundColor(Color.argb(180, 0, 0, 0))
            setPadding(16, 8, 16, 8)
        }
        val roomView = RoomView(this@MainActivity, inputController)
        val root = FrameLayout(this).apply {
            addView(roomView)
            addView(
                title,
                FrameLayout.LayoutParams(
                    FrameLayout.LayoutParams.WRAP_CONTENT,
                    FrameLayout.LayoutParams.WRAP_CONTENT,
                    Gravity.TOP or Gravity.CENTER_HORIZONTAL,
                ),
            )
            addView(
                hints,
                FrameLayout.LayoutParams(
                    FrameLayout.LayoutParams.WRAP_CONTENT,
                    FrameLayout.LayoutParams.WRAP_CONTENT,
                    Gravity.BOTTOM or Gravity.CENTER_HORIZONTAL,
                ),
            )
        }
        setContentView(root)
    }

    // Diagnostic-mode path: no live cursor, no room view, no input handling -
    // just a status line while file_replay.cpp streams the file on a
    // background thread (nativePlayEac3File blocks until the whole file has
    // drained, so it must never run on the main/UI thread).
    private fun startFileReplay(path: String) {
        val status = TextView(this).apply {
            text = "Diagnostic file replay\n\n$path\n\nstreaming… (see logcat " +
                "tag ac3forge.shield.file_replay)"
            textSize = 20f
            gravity = Gravity.CENTER
        }
        setContentView(status)
        thread(name = "file-replay") {
            val ok = try {
                NativeBridge.nativePlayEac3File(path)
            } catch (e: UnsatisfiedLinkError) {
                Log.e(TAG, "file replay failed to start", e)
                false
            }
            Log.i(TAG, "file replay finished: ok=$ok")
            runOnUiThread {
                status.text = "Diagnostic file replay\n\n$path\n\n" +
                    (if (ok) "done - see logcat for burst stats" else "FAILED - see logcat")
            }
        }
    }

    override fun onResume() {
        super.onResume()
        inputController.start()
    }

    override fun onPause() {
        inputController.stop()
        super.onPause()
    }

    override fun onDestroy() {
        NativeBridge.nativeStopLiveCursor()
        passthroughBridge.close()
        super.onDestroy()
    }

    override fun onGenericMotionEvent(event: MotionEvent): Boolean {
        if (inputController.onGenericMotionEvent(event)) {
            return true
        }
        return super.onGenericMotionEvent(event)
    }

    override fun onKeyDown(keyCode: Int, keyEvent: KeyEvent?): Boolean {
        if (keyEvent != null && inputController.onKeyDown(keyCode, keyEvent)) {
            return true
        }
        return super.onKeyDown(keyCode, keyEvent)
    }

    override fun onKeyUp(keyCode: Int, keyEvent: KeyEvent?): Boolean {
        if (inputController.onKeyUp(keyCode)) {
            return true
        }
        return super.onKeyUp(keyCode, keyEvent)
    }

    override fun onKeyLongPress(keyCode: Int, keyEvent: KeyEvent?): Boolean {
        if (inputController.onKeyLongPress(keyCode)) {
            return true
        }
        return super.onKeyLongPress(keyCode, keyEvent)
    }
}
