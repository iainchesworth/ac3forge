package com.ac3forge.shield

import android.content.Context
import android.graphics.Canvas
import android.graphics.Color
import android.graphics.DashPathEffect
import android.graphics.Paint
import android.graphics.Path
import android.util.AttributeSet
import android.view.Choreographer
import android.view.View

/**
 * The v1 room visualization: a plain [View], not a `SurfaceView` - the plan's
 * own justification for a 2.5D `Canvas` approach at all ("a real-time
 * position dashboard with no lighting/occlusion/camera-navigation needs")
 * applies just as much to skipping `SurfaceView`'s own render thread and
 * `SurfaceHolder` lifecycle: a handful of `drawCircle`/`drawLine` calls per
 * frame is nowhere near enough work to justify that complexity, and
 * `postInvalidateOnAnimation`-driven `View.onDraw` already runs on the
 * Choreographer-synced UI thread vsync callback the plan asked for.
 *
 * Draws two panels side by side, both reading the same
 * [NativeBridge.nativeGetObjectState] snapshot the encode loop
 * (live_cursor.cpp's LiveCursorState) just built for this frame:
 *  - left: top-down, room x (left/right) against room y (front/back)
 *  - right: side elevation, room x (left/right) against room z (floor/ceiling)
 * matching oamd.hpp's Position contract (x,y in [0,1], z in [-1,1]) - see
 * live_cursor.cpp's LiveCursorState::deflect_selected clamp.
 *
 * A few demoability additions on top of the raw positions: a listener marker
 * at the room's exact centre (where the JOC/VBAP render implicitly assumes
 * the listener sits - see live_cursor.cpp's trajectory_position comment), a
 * faint guide circle showing the lead object's planned orbit so a viewer can
 * see it being pushed off course and springing back rather than just seeing
 * a dot move, and a live axis-mode readout from [inputController].
 */
class RoomView @JvmOverloads constructor(
    context: Context,
    private val inputController: InputController? = null,
    attrs: AttributeSet? = null,
) : View(context, attrs) {

    private val choreographer = Choreographer.getInstance()
    private val frameCallback = object : Choreographer.FrameCallback {
        override fun doFrame(frameTimeNanos: Long) {
            invalidate()
            if (isAttachedToWindow) {
                choreographer.postFrameCallback(this)
            }
        }
    }

    private val roomPaint = Paint(Paint.ANTI_ALIAS_FLAG).apply {
        style = Paint.Style.STROKE
        strokeWidth = 3f
        color = Color.DKGRAY
    }
    private val labelPaint = Paint(Paint.ANTI_ALIAS_FLAG).apply {
        color = Color.LTGRAY
        textSize = 36f
    }
    private val objectPaint = Paint(Paint.ANTI_ALIAS_FLAG).apply {
        style = Paint.Style.FILL
    }
    private val selectedRingPaint = Paint(Paint.ANTI_ALIAS_FLAG).apply {
        style = Paint.Style.STROKE
        strokeWidth = 5f
        color = Color.WHITE
    }
    private val guidePaint = Paint(Paint.ANTI_ALIAS_FLAG).apply {
        style = Paint.Style.STROKE
        strokeWidth = 2f
        color = Color.argb(120, 229, 57, 53)  // faint version of the lead object's color
        pathEffect = DashPathEffect(floatArrayOf(10f, 10f), 0f)
    }
    private val listenerPaint = Paint(Paint.ANTI_ALIAS_FLAG).apply {
        style = Paint.Style.FILL
        color = Color.WHITE
    }
    private val modePaint = Paint(Paint.ANTI_ALIAS_FLAG).apply {
        color = Color.rgb(251, 192, 45)  // matches kObjectColors' amber, an unused hue here
        textSize = 32f
    }

    override fun onAttachedToWindow() {
        super.onAttachedToWindow()
        choreographer.postFrameCallback(frameCallback)
    }

    override fun onDetachedFromWindow() {
        choreographer.removeFrameCallback(frameCallback)
        super.onDetachedFromWindow()
    }

    override fun onDraw(canvas: Canvas) {
        super.onDraw(canvas)

        val state = try {
            NativeBridge.nativeGetObjectState()
        } catch (e: UnsatisfiedLinkError) {
            return
        }
        val objectCount = state.size / 4
        if (objectCount == 0) return

        val panelGap = 24f
        val panelWidth = (width - panelGap) / 2f
        val panelHeight = height.toFloat()

        drawPanel(
            canvas,
            left = 0f,
            top = 0f,
            right = panelWidth,
            bottom = panelHeight,
            title = "Top-down (X/Y)",
            state = state,
            objectCount = objectCount,
            horizontal = { i -> state[i * 4] },      // x
            vertical = { i -> state[i * 4 + 1] },     // y, [0,1]
            verticalIsHeight = false,
            showTrajectoryGuide = true,
        )
        drawPanel(
            canvas,
            left = panelWidth + panelGap,
            top = 0f,
            right = panelWidth + panelGap + panelWidth,
            bottom = panelHeight,
            title = "Side elevation (X/Z)",
            state = state,
            objectCount = objectCount,
            horizontal = { i -> state[i * 4] },           // x
            vertical = { i -> (state[i * 4 + 2] + 1f) / 2f }, // z in [-1,1] -> [0,1]
            verticalIsHeight = true,
            showTrajectoryGuide = false,
        )

        inputController?.let { controller ->
            val modeText = if (controller.axisMode == InputController.AxisMode.XY) {
                "D-pad height mode: OFF (up/down = depth)"
            } else {
                "D-pad height mode: ON (up/down = height)"
            }
            canvas.drawText(modeText, 24f, height - 16f, modePaint)
        }
    }

    private inline fun drawPanel(
        canvas: Canvas,
        left: Float,
        top: Float,
        right: Float,
        bottom: Float,
        title: String,
        state: FloatArray,
        objectCount: Int,
        horizontal: (Int) -> Float,
        vertical: (Int) -> Float,
        verticalIsHeight: Boolean,
        showTrajectoryGuide: Boolean,
    ) {
        val labelHeight = 48f
        val margin = 32f
        val roomLeft = left + margin
        val roomTop = top + labelHeight + margin
        val roomRight = right - margin
        val roomBottom = bottom - margin
        if (roomRight <= roomLeft || roomBottom <= roomTop) return

        canvas.drawText(title, left, top + labelHeight - 12f, labelPaint)
        canvas.drawRect(roomLeft, roomTop, roomRight, roomBottom, roomPaint)

        val roomWidth = roomRight - roomLeft
        val roomHeight = roomBottom - roomTop
        val radius = minOf(roomWidth, roomHeight) * 0.03f

        // The lead object's planned orbit (live_cursor.cpp's kTrajectory[0]:
        // radius 0.45 about the room's exact centre) - a static guide so a
        // viewer can see it being pushed off this course and drifting back,
        // not just a dot moving with no reference. Duplicated as a constant
        // rather than queried from native because it never changes at
        // runtime; kept in sync by the comment on both ends.
        if (showTrajectoryGuide) {
            val guideLeft = roomLeft + (0.5f - kTrajectoryGuideRadius) * roomWidth
            val guideRight = roomLeft + (0.5f + kTrajectoryGuideRadius) * roomWidth
            val guideTop = roomTop + (0.5f - kTrajectoryGuideRadius) * roomHeight
            val guideBottom = roomTop + (0.5f + kTrajectoryGuideRadius) * roomHeight
            canvas.drawOval(guideLeft, guideTop, guideRight, guideBottom, guidePaint)
        }

        // The listener: both panels' exact centre is (0.5, 0.5) in normalized
        // room-fraction space - room centre for the top-down panel, x=0.5 at
        // ear height (z=0) for the elevation panel - which is where the
        // JOC/VBAP render implicitly assumes the listener sits. A small
        // diamond rather than a circle so it never reads as just another
        // object.
        run {
            val lx = roomLeft + 0.5f * roomWidth
            val ly = roomTop + 0.5f * roomHeight
            val s = radius * 0.6f
            val path = Path().apply {
                moveTo(lx, ly - s)
                lineTo(lx + s, ly)
                lineTo(lx, ly + s)
                lineTo(lx - s, ly)
                close()
            }
            canvas.drawPath(path, listenerPaint)
        }

        for (i in 0 until objectCount) {
            val isSelected = state[i * 4 + 3] != 0f
            val nx = horizontal(i).coerceIn(0f, 1f)
            // Screen y grows downward; "up" on screen should be further from
            // camera (top-down panel) or higher/toward ceiling (elevation
            // panel), so invert. Elevation panel's floor is naturally the
            // bottom of the rect either way, which this also gives.
            val ny = 1f - vertical(i).coerceIn(0f, 1f)

            val px = roomLeft + nx * roomWidth
            val py = roomTop + ny * roomHeight

            objectPaint.color = objectColor(i)
            canvas.drawCircle(px, py, radius, objectPaint)
            if (isSelected) {
                canvas.drawCircle(px, py, radius + 8f, selectedRingPaint)
            }
        }

        if (verticalIsHeight) {
            canvas.drawText("ceiling", roomLeft, roomTop - 8f, labelPaint)
            canvas.drawText("floor", roomLeft, roomBottom + 40f, labelPaint)
        }
    }

    companion object {
        // live_cursor.cpp's kTrajectory[0].radius - the lead (interactive)
        // object's orbit radius about the room centre. Kept as a duplicated
        // constant, not queried over JNI, because it is fixed at compile
        // time on the native side too; if that value ever changes, update
        // this one to match.
        private const val kTrajectoryGuideRadius = 0.45f

        // Distinct, high-contrast hues; cycles if ever more objects than
        // colors, but kObjects (3, see live_cursor.cpp) is well under this.
        private val kObjectColors = intArrayOf(
            Color.rgb(229, 57, 53),   // red
            Color.rgb(67, 160, 71),   // green
            Color.rgb(30, 136, 229),  // blue
            Color.rgb(251, 192, 45),  // amber
        )

        private fun objectColor(index: Int): Int = kObjectColors[index % kObjectColors.size]
    }
}
