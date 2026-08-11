package com.ac3forge.shield

import android.view.Choreographer
import android.view.InputDevice
import android.view.KeyEvent
import android.view.MotionEvent
import kotlin.math.abs

/**
 * Shield Controller (analog sticks + shoulder buttons) and basic Shield
 * remote (D-pad only) input, both mapped to the same
 * [NativeBridge.nativeMoveSelectedObject]/[NativeBridge.nativeCycleSelectedObject]
 * calls - see live_cursor.cpp's LiveCursorState for what actually consumes
 * these.
 *
 * Two different input shapes, deliberately handled differently:
 *  - Analog (left stick x/y, right stick y for height): continuous,
 *    accumulated into [stickX]/[stickY]/[stickZ] by [onGenericMotionEvent]
 *    and applied once per animation frame by a [Choreographer] callback,
 *    scaled by elapsed time - NOT once per raw MotionEvent, which would
 *    move in per-event jumps rather than smoothly.
 *  - Digital (D-pad, shoulder buttons, A/center): a fixed-size nudge
 *    applied directly in [onKeyDown], one-shot per press. This is the only
 *    control scheme the basic remote has at all, and it works identically
 *    whether or not a Shield Controller happens to also be connected.
 *
 * No explicit InputDevice source detection/hot-plug listener: both schemes
 * are handled reactively as events arrive (onGenericMotionEvent only ever
 * fires for an analog-capable source in the first place), so there is
 * nothing that needs to know in advance which device is paired.
 */
class InputController {
    @Volatile private var stickX = 0f
    @Volatile private var stickY = 0f
    @Volatile private var stickZ = 0f

    private var running = false
    private var lastFrameTimeNanos = 0L
    private val choreographer = Choreographer.getInstance()

    private val frameCallback = object : Choreographer.FrameCallback {
        override fun doFrame(frameTimeNanos: Long) {
            if (!running) return
            if (lastFrameTimeNanos != 0L) {
                val dtSeconds = (frameTimeNanos - lastFrameTimeNanos) / 1_000_000_000f
                applyContinuousMovement(dtSeconds)
            }
            lastFrameTimeNanos = frameTimeNanos
            choreographer.postFrameCallback(this)
        }
    }

    /** Call from Activity.onResume. */
    fun start() {
        if (running) return
        running = true
        lastFrameTimeNanos = 0L
        choreographer.postFrameCallback(frameCallback)
    }

    /** Call from Activity.onPause. */
    fun stop() {
        running = false
        choreographer.removeFrameCallback(frameCallback)
        stickX = 0f
        stickY = 0f
        stickZ = 0f
    }

    private fun applyContinuousMovement(dtSeconds: Float) {
        if (stickX == 0f && stickY == 0f && stickZ == 0f) return
        NativeBridge.nativeMoveSelectedObject(
            stickX * ANALOG_SPEED * dtSeconds,
            stickY * ANALOG_SPEED * dtSeconds,
            stickZ * ANALOG_SPEED * dtSeconds,
        )
    }

    /** Call from Activity.onGenericMotionEvent. Returns true if handled. */
    fun onGenericMotionEvent(event: MotionEvent): Boolean {
        if (event.source and InputDevice.SOURCE_JOYSTICK != InputDevice.SOURCE_JOYSTICK) {
            return false
        }
        stickX = deadzone(event.getAxisValue(MotionEvent.AXIS_X))
        // oamd.hpp's room y runs front (0) to back (1); MotionEvent's AXIS_Y
        // is positive pushing the stick DOWN/back on the physical pad, which
        // reads naturally as "into the room" already pointed the right way,
        // so this is NOT flipped - only AXIS_Z (right stick vertical) is,
        // since pushing that stick UP should raise the object, the opposite
        // of AXIS_Z's own positive-is-down convention.
        stickY = deadzone(event.getAxisValue(MotionEvent.AXIS_Y))
        stickZ = deadzone(-event.getAxisValue(MotionEvent.AXIS_Z))
        return true
    }

    /** Call from Activity.onKeyDown. Returns true if handled. */
    fun onKeyDown(keyCode: Int): Boolean {
        return when (keyCode) {
            KeyEvent.KEYCODE_DPAD_LEFT -> {
                NativeBridge.nativeMoveSelectedObject(-DPAD_STEP, 0f, 0f)
                true
            }
            KeyEvent.KEYCODE_DPAD_RIGHT -> {
                NativeBridge.nativeMoveSelectedObject(DPAD_STEP, 0f, 0f)
                true
            }
            // See onGenericMotionEvent's comment: "up" on the D-pad means
            // further into the room, same non-flipped sense as AXIS_Y.
            KeyEvent.KEYCODE_DPAD_UP -> {
                NativeBridge.nativeMoveSelectedObject(0f, DPAD_STEP, 0f)
                true
            }
            KeyEvent.KEYCODE_DPAD_DOWN -> {
                NativeBridge.nativeMoveSelectedObject(0f, -DPAD_STEP, 0f)
                true
            }
            KeyEvent.KEYCODE_DPAD_CENTER, KeyEvent.KEYCODE_ENTER,
            KeyEvent.KEYCODE_BUTTON_A -> {
                NativeBridge.nativeCycleSelectedObject()
                true
            }
            // Height (z) has no D-pad equivalent at all - the basic remote
            // genuinely cannot control it, only the Shield Controller's
            // shoulder buttons can. Documented limitation, not an oversight;
            // see docs/platforms/android.md.
            KeyEvent.KEYCODE_BUTTON_L1 -> {
                NativeBridge.nativeMoveSelectedObject(0f, 0f, DPAD_STEP)
                true
            }
            KeyEvent.KEYCODE_BUTTON_R1 -> {
                NativeBridge.nativeMoveSelectedObject(0f, 0f, -DPAD_STEP)
                true
            }
            else -> false
        }
    }

    private fun deadzone(value: Float): Float = if (abs(value) < DEADZONE) 0f else value

    companion object {
        // Below this, a stick's own physical rest-position noise/drift would
        // otherwise register as constant tiny movement.
        private const val DEADZONE = 0.15f
        // Room-fraction per second at full stick deflection - roughly 1.7s
        // to cross the whole room edge to edge, a deliberately unhurried
        // pace for a demo meant to be watched as much as played.
        private const val ANALOG_SPEED = 0.6f
        // Room-fraction per D-pad/shoulder-button press - about 20 presses
        // edge to edge, matching ANALOG_SPEED's overall feel at a
        // comfortable button-mash rate.
        private const val DPAD_STEP = 0.05f
    }
}
