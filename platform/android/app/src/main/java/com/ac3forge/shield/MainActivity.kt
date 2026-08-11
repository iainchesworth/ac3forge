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

private const val TAG = "ShieldAtmosDemo"

/**
 * Loads the native library, registers the [PassthroughBridge], runs the
 * HDMI capability probe, starts the encode loop (live_cursor.cpp), wires
 * Shield Controller/remote input through [InputController], and shows the
 * live object positions via [RoomView] - the startup/capability report stays
 * as a one-shot logcat entry plus a small on-screen control-hints overlay
 * rather than replacing the room view, since it doesn't change frame to
 * frame the way the room view does.
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

        val liveCursorStarted = try {
            NativeBridge.nativeStartLiveCursor()
        } catch (e: UnsatisfiedLinkError) {
            Log.e(TAG, "encode loop failed to start", e)
            false
        }
        Log.i(TAG, "encode loop started: $liveCursorStarted")

        val hints = TextView(this).apply {
            text = "Left stick / D-pad: move object (x/y)   " +
                "Right stick / L1+R1: height (z)   " +
                "A / D-pad center: select next object"
            textSize = 14f
            gravity = Gravity.CENTER
            setTextColor(Color.LTGRAY)
            setBackgroundColor(Color.argb(180, 0, 0, 0))
            setPadding(16, 8, 16, 8)
        }
        val root = FrameLayout(this).apply {
            addView(RoomView(this@MainActivity))
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
        if (inputController.onKeyDown(keyCode)) {
            return true
        }
        return super.onKeyDown(keyCode, keyEvent)
    }
}
