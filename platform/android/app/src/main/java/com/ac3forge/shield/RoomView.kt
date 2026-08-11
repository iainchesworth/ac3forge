package com.ac3forge.shield

import android.content.Context
import android.graphics.Canvas
import android.graphics.Color
import android.graphics.Paint
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
 * live_cursor.cpp's LiveCursorState::move_selected clamp.
 */
class RoomView @JvmOverloads constructor(
    context: Context,
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
        )
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
