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
import kotlin.math.abs

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
 * Three panels, all reading the same [NativeBridge.nativeGetObjectState]
 * snapshot the encode loop (live_cursor.cpp's LiveCursorState) just built
 * for this frame:
 *  - top-right: top-down, room x (left/right) against room y (front/back)
 *  - bottom-right: side elevation, room x (left/right) against room z
 *    (floor/ceiling)
 *  - left (the bigger panel): a tilted isometric 3D view showing all three
 *    axes at once, plus the lead object's own trail through space - see
 *    [draw3DView]
 * matching oamd.hpp's Position contract (x,y in [0,1], z in [-1,1]) - see
 * live_cursor.cpp's LiveCursorState::deflect_selected clamp.
 *
 * A few demoability additions on top of the raw positions: a listener marker
 * at the room's exact centre (where the JOC/VBAP render implicitly assumes
 * the listener sits - see live_cursor.cpp's trajectory_position comment), a
 * faint guide circle showing the lead object's planned orbit so a viewer can
 * see it being pushed off course and springing back rather than just seeing
 * a dot move, and live axis-mode/stream-stats readouts.
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
    private val statsPaint = Paint(Paint.ANTI_ALIAS_FLAG).apply {
        color = Color.rgb(120, 200, 255)  // a cool blue, distinct from every other overlay's hue
        textSize = 32f
    }
    private val trailPaint = Paint(Paint.ANTI_ALIAS_FLAG).apply {
        style = Paint.Style.STROKE
        strokeWidth = 4f
        color = Color.WHITE
    }
    private val dropLinePaint = Paint(Paint.ANTI_ALIAS_FLAG).apply {
        style = Paint.Style.STROKE
        strokeWidth = 2f
        color = Color.WHITE
    }

    // The lead object's own recent-position history for the 3D view's trail
    // ("where it's come from") - RoomView's own responsibility, not
    // native's: this is the object's REAL, actually-traversed path
    // (deflection included), sampled once per draw call, capped at
    // MAX_HISTORY entries FIFO. The "where it's going" half of the same
    // trail is queried fresh from native each frame instead (see
    // draw3DView) - the base trajectory, no deflection, since future
    // deflection can't be known.
    private val leadHistory = ArrayDeque<FloatArray>()

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

        leadHistory.addLast(floatArrayOf(state[0], state[1], state[2]))
        while (leadHistory.size > MAX_HISTORY) {
            leadHistory.removeFirst()
        }
        val leadFuture = try {
            NativeBridge.nativeGetFutureLeadTrajectory(FUTURE_SECONDS, FUTURE_SAMPLES)
        } catch (e: UnsatisfiedLinkError) {
            null
        }

        // Left column (wider - the isometric view is the most information-
        // dense of the three, so it gets the most screen): the 3D trail
        // view. Right column: the original two panels, stacked, sized close
        // to square (rightWidth near each stacked panel's own height) -
        // rather than the wide, short rectangles a fixed width fraction
        // produced - so the 3D view gets whatever width is left over
        // instead of a fixed, arbitrary split. Clamped so neither extreme
        // device aspect ratio produces a degenerate column.
        val columnGap = 24f
        val rowGap = 24f
        val rightPanelHeight = (height - rowGap) / 2f
        val rightWidth = rightPanelHeight.coerceIn(width * 0.22f, width * 0.4f)
        val leftWidth = width - rightWidth - columnGap
        val rightLeft = leftWidth + columnGap

        draw3DView(canvas, 0f, 0f, leftWidth, height.toFloat(), state, objectCount, leadFuture)

        drawPanel(
            canvas,
            left = rightLeft,
            top = 0f,
            right = rightLeft + rightWidth,
            bottom = rightPanelHeight,
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
            left = rightLeft,
            top = rightPanelHeight + rowGap,
            right = rightLeft + rightWidth,
            bottom = rightPanelHeight * 2f + rowGap,
            title = "Side elevation (X/Z)",
            state = state,
            objectCount = objectCount,
            horizontal = { i -> state[i * 4] },           // x
            vertical = { i -> (state[i * 4 + 2] + 1f) / 2f }, // z in [-1,1] -> [0,1]
            verticalIsHeight = true,
            showTrajectoryGuide = false,
        )

        // Top-right, not bottom-left: the bottom of the screen is already the
        // control-hints overlay (MainActivity's own TextView, added on top of
        // this view) - drawing another line of text there put this readout's
        // amber text directly behind the hints' own text, an unreadable
        // overlap on real hardware. Right-aligned against the screen's own
        // right edge, comfortably clear of the (left-aligned, starting
        // mid-screen) "Side elevation (X/Z)" panel title below it.
        inputController?.let { controller ->
            val modeText = if (controller.axisMode == InputController.AxisMode.XY) {
                "D-pad height mode: OFF (up/down = depth)"
            } else {
                "D-pad height mode: ON (up/down = height)"
            }
            // y=130f, not right under the panel titles (drawn at ~y=36 with a
            // 36f-tall font) - clear of both those and the app's own title
            // bar overlaid above this view.
            val textWidth = modePaint.measureText(modeText)
            canvas.drawText(modeText, width - textWidth - 24f, 130f, modePaint)
        }

        // Same row as the axis-mode readout above, opposite corner - the two
        // read as a matched pair rather than two unrelated overlays.
        val statsText = try {
            NativeBridge.nativeGetStreamStatsText()
        } catch (e: UnsatisfiedLinkError) {
            null
        }
        if (statsText != null) {
            canvas.drawText(statsText, 24f, 130f, statsPaint)
        }
    }

    /**
     * A tilted isometric projection ("2:1 video-game" style: x and y both
     * project onto diagonal screen directions, z projects straight up) so
     * all three room axes are visible in one view at once, per the brief:
     * "tilted down so you can see all three axes." Draws a floor-plan
     * wireframe for spatial reference, the lead object's trail (recent
     * history behind it, planned course ahead of it - see [leadHistory] and
     * `future`) with a drop-line from each trail point down to the floor
     * directly below it (so height reads as an unambiguous vertical offset,
     * not just a diagonal shift easy to misjudge in an oblique projection),
     * fading to transparent with distance from "now" in either direction,
     * and every object's current position as a solid dot, matching the
     * other two panels.
     */
    private fun draw3DView(
        canvas: Canvas,
        left: Float,
        top: Float,
        right: Float,
        bottom: Float,
        state: FloatArray,
        objectCount: Int,
        future: FloatArray?,
    ) {
        val labelHeight = 48f
        val margin = 32f
        val panelLeft = left + margin
        val panelTop = top + labelHeight + margin
        val panelRight = right - margin
        val panelBottom = bottom - margin
        if (panelRight <= panelLeft || panelBottom <= panelTop) return

        canvas.drawText("3D track", left, top + labelHeight - 12f, labelPaint)

        val panelW = panelRight - panelLeft
        val panelH = panelBottom - panelTop
        val centerX = panelLeft + panelW / 2f
        val centerY = panelTop + panelH / 2f
        val scale = minOf(
            (panelW / 2f - 24f) / ISO_X_HALF_RANGE,
            (panelH / 2f - 24f) / ISO_Y_HALF_RANGE,
        )

        fun project(x: Float, y: Float, z: Float): FloatArray {
            val cx = x - 0.5f
            val cy = y - 0.5f
            val isoX = (cx - cy) * ISO_COS30
            val isoY = (cx + cy) * ISO_SIN30 - z * ISO_Z_SCALE
            return floatArrayOf(centerX + isoX * scale, centerY + isoY * scale)
        }

        // Floor wireframe: the room's four corners at z = -1, so the trail's
        // drop-lines below have a visible surface to land on.
        val floorCorners = arrayOf(
            floatArrayOf(0f, 0f), floatArrayOf(1f, 0f),
            floatArrayOf(1f, 1f), floatArrayOf(0f, 1f),
        )
        for (i in floorCorners.indices) {
            val a = project(floorCorners[i][0], floorCorners[i][1], -1f)
            val b = project(floorCorners[(i + 1) % floorCorners.size][0],
                floorCorners[(i + 1) % floorCorners.size][1], -1f)
            canvas.drawLine(a[0], a[1], b[0], b[1], roomPaint)
        }

        // The trail: history (already-traversed, real positions) then "now"
        // then future (planned course ahead, no deflection). A single
        // continuous line through all of it, each point's alpha fading with
        // its distance from "now" in samples.
        val historySize = leadHistory.size
        val trail = ArrayList<FloatArray>(historySize + (future?.size ?: 0) / 3)
        trail.addAll(leadHistory)
        if (future != null) {
            var i = 0
            while (i + 2 < future.size) {
                trail.add(floatArrayOf(future[i], future[i + 1], future[i + 2]))
                i += 3
            }
        }
        var prevX = Float.NaN
        var prevY = Float.NaN
        for ((index, p) in trail.withIndex()) {
            val distanceFromNow = abs(index - historySize)
            val alpha = (255 - (distanceFromNow * 255 / TRAIL_FADE_SAMPLES)).coerceIn(0, 255)
            if (alpha == 0) {
                prevX = Float.NaN
                continue
            }
            val (px, py) = project(p[0], p[1], p[2]).let { it[0] to it[1] }
            val (floorX, floorY) = project(p[0], p[1], -1f).let { it[0] to it[1] }
            dropLinePaint.alpha = alpha / 3
            canvas.drawLine(px, py, floorX, floorY, dropLinePaint)
            if (!prevX.isNaN()) {
                trailPaint.alpha = alpha
                canvas.drawLine(prevX, prevY, px, py, trailPaint)
            }
            prevX = px
            prevY = py
        }

        // Current positions, every object - matches the other two panels.
        for (i in 0 until objectCount) {
            val isSelected = state[i * 4 + 3] != 0f
            val (px, py) = project(state[i * 4], state[i * 4 + 1], state[i * 4 + 2])
                .let { it[0] to it[1] }
            objectPaint.color = objectColor(i)
            canvas.drawCircle(px, py, 16f, objectPaint)
            if (isSelected) {
                canvas.drawCircle(px, py, 24f, selectedRingPaint)
            }
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
        val radius = minOf(roomWidth, roomHeight) * 0.05f

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
            // roomBottom + 24f, not +40f: for the bottom-most stacked panel,
            // roomBottom + margin (32f) is this View's own last pixel row -
            // +40f drew past it and got clipped, confirmed on a real device
            // screenshot. +24f comfortably fits the label within margin.
            canvas.drawText("floor", roomLeft, roomBottom + 24f, labelPaint)
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

        // Classic "2:1 video-game" isometric projection constants: x and y
        // both project onto 30-degree diagonal screen directions, z
        // projects straight up/down - see draw3DView's own comment.
        private const val ISO_COS30 = 0.8660254f
        private const val ISO_SIN30 = 0.5f
        private const val ISO_Z_SCALE = 0.6f
        // Analytically-derived extents of project()'s output over the whole
        // room+height range (x,y in [0,1], z in [-1,1]) - used to fit the
        // projection into a panel of any size without iterating every frame.
        private const val ISO_X_HALF_RANGE = ISO_COS30
        private const val ISO_Y_HALF_RANGE = ISO_SIN30 + ISO_Z_SCALE

        // How many of the lead's own past positions the 3D view's trail
        // keeps (see leadHistory) - a sample count, not a fixed time window,
        // so it stays simple across whatever frame rate the device actually
        // renders at.
        private const val MAX_HISTORY = 150
        // How many trail samples from "now" (in either direction) until a
        // point fades to fully transparent.
        private const val TRAIL_FADE_SAMPLES = 90
        // How far into the future (and how many samples of it) the trail's
        // "path ahead" half queries from native each frame.
        private const val FUTURE_SECONDS = 4f
        private const val FUTURE_SAMPLES = 90
    }
}
