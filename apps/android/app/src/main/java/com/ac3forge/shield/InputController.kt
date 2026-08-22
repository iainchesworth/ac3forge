package com.ac3forge.shield

import android.view.Choreographer
import android.view.InputDevice
import android.view.KeyEvent
import android.view.MotionEvent
import kotlin.math.abs

/**
 * Shield Controller (analog sticks + shoulder buttons + D-pad) and basic
 * Shield remote (D-pad only) input, both mapped to the same
 * [NativeBridge.nativeDeflectSelectedObject]/[NativeBridge.nativeSnapSelectedToCourse]
 * calls - see live_cursor.cpp's LiveCursorState for what actually consumes
 * these. Every input source here BIASES the selected object off its own
 * pre-planned trajectory rather than moving it outright; release the input
 * and native's own per-frame decay carries it back onto that trajectory on
 * its own (see LiveCursorState::advance) - nothing here has to detect "input
 * stopped" itself, it just stops calling in and the bias decays regardless.
 *
 * Two different input shapes, both continuous while held now, unified into
 * one accumulated bias vector applied once per animation frame
 * ([applyContinuousMovement], via a [Choreographer] callback) rather than
 * once per raw event - a per-event step would move in per-event jumps:
 *  - Analog (left stick x/y, right stick y for height): [stickX]/[stickY]/
 *    [stickZ], set continuously by [onGenericMotionEvent].
 *  - Digital (D-pad, shoulder buttons): [dpadX]/[dpadSecondary]/[shoulderZ],
 *    each pinned to -1/0/+1 for as long as the corresponding key is held,
 *    set in [onKeyDown] and cleared in [onKeyUp]. This is the ONLY control
 *    scheme the basic remote has at all, and it works identically whether or
 *    not a Shield Controller happens to also be connected.
 *
 * The D-pad's second axis (up/down) is [axisMode]-dependent: X/Y by default
 * (up biases the object toward the front wall/listener-facing direction,
 * down toward the back - room-anchored, not screen- or stick-relative, and
 * matching the analog stick's already-correct sense of "push away from you"
 * meaning forward), or X/Z (up/down biases height) after toggling - the
 * remote's only way to reach height at all, since it has no second stick or
 * shoulder buttons. Toggled by a SHORT press of the centre/select button -
 * deliberately immediate, not gated behind a long-press hold: on the basic
 * remote this is the ONLY way to reach height, and real-device testing found
 * a long-press-to-switch felt sluggish for something that needs switching
 * back and forth rapidly while actively shaping a path in real time. A LONG
 * press of the same button instead instantly snaps the selected object back
 * onto its planned course - a presenter's "and... reset" button, rather than
 * waiting out the usual ~1.5s spring-back decay - see
 * [onKeyLongPress]/[onKeyUp]'s disambiguation.
 *
 * No explicit InputDevice source detection/hot-plug listener: both schemes
 * are handled reactively as events arrive (onGenericMotionEvent only ever
 * fires for an analog-capable source in the first place), so there is
 * nothing that needs to know in advance which device is paired.
 *
 * The remote's dedicated play/pause keys mute/unmute the two ambient
 * objects (see [NativeBridge.nativeSetAmbientMuted]) - added after
 * real-device testing found three simultaneous tones made it hard to
 * precisely localize the lead object's own movement by ear. Pausing lets a
 * listener isolate it; playing brings the ambient wash back.
 */
class InputController {
    @Volatile private var stickX = 0f
    @Volatile private var stickY = 0f
    @Volatile private var stickZ = 0f
    @Volatile private var dpadX = 0f
    @Volatile private var dpadSecondary = 0f
    @Volatile private var shoulderZ = 0f

    @Volatile var axisMode: AxisMode = AxisMode.XY
        private set

    @Volatile private var ambientMuted = false

    /**
     * Invoked once per handled input event (stick motion, D-pad, buttons) -
     * never for events this controller doesn't recognize. Set by
     * MainActivity to dismiss the first-launch orientation cue on first real
     * input and to reset the idle/attract-prompt timer. Deliberately a plain
     * callback rather than this class owning any idle/UI-timing logic itself
     * - that's presentation concern, not input-mapping concern.
     */
    var onInputActivity: (() -> Unit)? = null

    // Set by onKeyLongPress, read (and cleared) by onKeyUp: the framework
    // calls onKeyDown once immediately and, if still held past the long-press
    // threshold, onKeyLongPress - onKeyUp always follows eventually either
    // way, so this is what tells it whether the long-press action already
    // fired (snap back to course) or the release should still count as
    // a short press (toggle axis mode).
    private var longPressConsumed = false

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
        dpadX = 0f
        dpadSecondary = 0f
        shoulderZ = 0f
    }

    private fun applyContinuousMovement(dtSeconds: Float) {
        val dpadY = if (axisMode == AxisMode.XY) dpadSecondary else 0f
        val dpadZ = if (axisMode == AxisMode.XZ) dpadSecondary else 0f
        val totalX = stickX + dpadX
        val totalY = stickY + dpadY
        val totalZ = stickZ + dpadZ + shoulderZ
        if (totalX == 0f && totalY == 0f && totalZ == 0f) return
        NativeBridge.nativeDeflectSelectedObject(
            totalX * INPUT_SPEED * dtSeconds,
            totalY * INPUT_SPEED * dtSeconds,
            totalZ * INPUT_SPEED * dtSeconds,
        )
    }

    /** Call from Activity.onGenericMotionEvent. Returns true if handled. */
    fun onGenericMotionEvent(event: MotionEvent): Boolean {
        if (event.source and InputDevice.SOURCE_JOYSTICK != InputDevice.SOURCE_JOYSTICK) {
            return false
        }
        stickX = deadzone(event.getAxisValue(MotionEvent.AXIS_X))
        // oamd.hpp's room y runs front (0, where the listener faces - see
        // Position's own comment) to back (1). MotionEvent's AXIS_Y is
        // positive pushing the stick DOWN/toward the player, which already
        // reads as "pull it toward me" = backward = increasing y, so this is
        // NOT flipped - only AXIS_Z (right stick vertical) is, since pushing
        // that stick UP should raise the object, the opposite of AXIS_Z's
        // own positive-is-down convention. The D-pad's UP/DOWN keys (below)
        // needed the equivalent flip applied explicitly, since a button has
        // no physical "pushed away from you" motion to read the sense from
        // the way a stick does.
        stickY = deadzone(event.getAxisValue(MotionEvent.AXIS_Y))
        stickZ = deadzone(-event.getAxisValue(MotionEvent.AXIS_Z))
        // Past the same deadzone real movement uses, not merely "an event
        // arrived" - some controllers deliver a low-level trickle of
        // ACTION_MOVE events even at physical rest, which would otherwise
        // defeat the whole point of the idle/attract-prompt timer this
        // drives.
        if (stickX != 0f || stickY != 0f || stickZ != 0f) {
            onInputActivity?.invoke()
        }
        return true
    }

    /**
     * Call from Activity.onKeyDown. `event` is needed (not just the key
     * code) for [KeyEvent.startTracking] below - Returns true if handled.
     */
    fun onKeyDown(keyCode: Int, event: KeyEvent): Boolean {
        val handled = onKeyDownInternal(keyCode, event)
        if (handled) onInputActivity?.invoke()
        return handled
    }

    private fun onKeyDownInternal(keyCode: Int, event: KeyEvent): Boolean {
        return when (keyCode) {
            KeyEvent.KEYCODE_DPAD_LEFT -> {
                dpadX = -1f
                true
            }
            KeyEvent.KEYCODE_DPAD_RIGHT -> {
                dpadX = 1f
                true
            }
            // Room-anchored, not "into the room": the listener sits facing
            // the front wall (oamd.hpp's y=0 - see Position's own comment),
            // the way anyone sitting in front of a screen actually faces -
            // so "up"/forward is TOWARD that wall (decreasing y), matching
            // both the analog stick (pushing it away from you already means
            // forward, unchanged below) and the top-down panel's own screen
            // layout (RoomView.kt draws the front wall at the top). This was
            // previously inverted here only, not on the stick - real-device
            // testing surfaced it as the two input methods disagreeing with
            // each other, not just "feels backwards." X/Z mode: up still
            // means higher, unaffected by any of this.
            KeyEvent.KEYCODE_DPAD_UP -> {
                dpadSecondary = -1f
                true
            }
            KeyEvent.KEYCODE_DPAD_DOWN -> {
                dpadSecondary = 1f
                true
            }
            // Otherwise a no-op beyond consuming the event: which action
            // this button performs is decided on release, once it's known
            // whether onKeyLongPress fired first - see that method and
            // onKeyUp. startTracking() is NOT automatic just because this
            // method returns true - KeyEvent.startTracking()'s own Javadoc is
            // explicit that onKeyLongPress only ever fires for a key this was
            // called on from onKeyDown; skipping it is a silent no-op long
            // press, not a degraded one.
            KeyEvent.KEYCODE_DPAD_CENTER, KeyEvent.KEYCODE_ENTER,
            KeyEvent.KEYCODE_BUTTON_A -> {
                event.startTracking()
                true
            }
            KeyEvent.KEYCODE_BUTTON_L1 -> {
                shoulderZ = 1f
                true
            }
            KeyEvent.KEYCODE_BUTTON_R1 -> {
                shoulderZ = -1f
                true
            }
            // repeatCount == 0 guards against the (unlikely, but not
            // impossible) case of the framework redelivering onKeyDown for a
            // held media key - these are handled here, on the down event,
            // rather than onKeyUp, so the mute/unmute takes effect the
            // instant the button is pressed rather than on release.
            KeyEvent.KEYCODE_MEDIA_PLAY_PAUSE -> {
                if (event.repeatCount == 0) {
                    ambientMuted = !ambientMuted
                    NativeBridge.nativeSetAmbientMuted(ambientMuted)
                }
                true
            }
            KeyEvent.KEYCODE_MEDIA_PAUSE -> {
                if (event.repeatCount == 0) {
                    ambientMuted = true
                    NativeBridge.nativeSetAmbientMuted(true)
                }
                true
            }
            KeyEvent.KEYCODE_MEDIA_PLAY -> {
                if (event.repeatCount == 0) {
                    ambientMuted = false
                    NativeBridge.nativeSetAmbientMuted(false)
                }
                true
            }
            else -> false
        }
    }

    /** Call from Activity.onKeyUp. Returns true if handled. */
    fun onKeyUp(keyCode: Int): Boolean {
        return when (keyCode) {
            KeyEvent.KEYCODE_DPAD_LEFT, KeyEvent.KEYCODE_DPAD_RIGHT -> {
                dpadX = 0f
                true
            }
            KeyEvent.KEYCODE_DPAD_UP, KeyEvent.KEYCODE_DPAD_DOWN -> {
                dpadSecondary = 0f
                true
            }
            KeyEvent.KEYCODE_DPAD_CENTER, KeyEvent.KEYCODE_ENTER,
            KeyEvent.KEYCODE_BUTTON_A -> {
                if (longPressConsumed) {
                    longPressConsumed = false
                } else {
                    axisMode = if (axisMode == AxisMode.XY) AxisMode.XZ else AxisMode.XY
                }
                true
            }
            KeyEvent.KEYCODE_BUTTON_L1, KeyEvent.KEYCODE_BUTTON_R1 -> {
                shoulderZ = 0f
                true
            }
            // Already actioned on the down event above; just consumed here
            // so the system's own (unused - this app has no MediaSession)
            // default key handling never sees them either.
            KeyEvent.KEYCODE_MEDIA_PLAY_PAUSE, KeyEvent.KEYCODE_MEDIA_PAUSE,
            KeyEvent.KEYCODE_MEDIA_PLAY -> true
            else -> false
        }
    }

    /**
     * Call from Activity.onKeyLongPress. Returns true if handled.
     *
     * A presenter's "and... reset" button: instantly snaps the selected
     * object back onto its planned course rather than waiting out the
     * usual ~1.5s spring-back decay. Replaces what used to be here (cycling
     * the selected object) - a no-op with a single interactive object, so
     * there was nothing real lost by repurposing the gesture.
     */
    fun onKeyLongPress(keyCode: Int): Boolean {
        return when (keyCode) {
            KeyEvent.KEYCODE_DPAD_CENTER, KeyEvent.KEYCODE_ENTER,
            KeyEvent.KEYCODE_BUTTON_A -> {
                NativeBridge.nativeSnapSelectedToCourse()
                longPressConsumed = true
                true
            }
            else -> false
        }
    }

    private fun deadzone(value: Float): Float = if (abs(value) < DEADZONE) 0f else value

    /** What the D-pad's up/down axis biases: further into the room, or height. */
    enum class AxisMode { XY, XZ }

    companion object {
        // Below this, a stick's own physical rest-position noise/drift would
        // otherwise register as constant tiny movement.
        private const val DEADZONE = 0.15f
        // Room-fraction per second at full stick deflection or a held D-pad
        // direction - roughly 1.7s to cross the whole room edge to edge, a
        // deliberately unhurried pace for a demo meant to be watched as much
        // as played. Shared by analog and digital input so the two feel the
        // same regardless of which device is in hand.
        private const val INPUT_SPEED = 0.6f
    }
}
