package com.ac3forge.shield

import android.content.Context
import android.graphics.Canvas
import android.graphics.Color
import android.graphics.DashPathEffect
import android.graphics.Paint
import android.graphics.Path
import android.graphics.RectF
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
 * Each panel is drawn as a rounded card (see [drawCard]) rather than a bare
 * stroked rectangle floating on the activity's own background - see
 * [Theme]'s own comment for why one shared palette exists at all.
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

    private val cardBgPaint = Paint(Paint.ANTI_ALIAS_FLAG).apply {
        style = Paint.Style.FILL
        color = Theme.colorSurface
    }
    private val cardBorderPaint = Paint(Paint.ANTI_ALIAS_FLAG).apply {
        style = Paint.Style.STROKE
        strokeWidth = 2f
        color = Theme.colorSurfaceBorder
    }
    private val dividerPaint = Paint(Paint.ANTI_ALIAS_FLAG).apply {
        style = Paint.Style.STROKE
        strokeWidth = 2f
        color = Theme.colorSurfaceBorder
    }
    private val roomPaint = Paint(Paint.ANTI_ALIAS_FLAG).apply {
        style = Paint.Style.STROKE
        strokeWidth = 2f
        color = Theme.colorSurfaceBorder
    }
    // Panel headers ("Top-down (X/Y)", "3D track", ...) - uppercase +
    // letter-spaced deliberately reads as a dashboard card caption, not
    // body text, so it stays visually subordinate to the objects moving
    // underneath it.
    private val titlePaint = Paint(Paint.ANTI_ALIAS_FLAG).apply {
        color = Theme.colorTextSecondary
        textSize = 26f
        letterSpacing = 0.08f
    }
    private val labelPaint = Paint(Paint.ANTI_ALIAS_FLAG).apply {
        color = Theme.colorTextMuted
        textSize = 30f
    }
    private val objectPaint = Paint(Paint.ANTI_ALIAS_FLAG).apply {
        style = Paint.Style.FILL
    }
    // A soft, low-alpha halo drawn beneath each object dot (reusing this one
    // Paint, mutating its color/alpha per object, rather than allocating a
    // RadialGradient shader per dot per frame - this view redraws at up to
    // 60fps and a fresh shader per dot per frame is needless GC pressure for
    // an effect two nested drawCircle calls achieves just as well).
    private val objectGlowPaint = Paint(Paint.ANTI_ALIAS_FLAG).apply {
        style = Paint.Style.FILL
    }
    private val selectedRingPaint = Paint(Paint.ANTI_ALIAS_FLAG).apply {
        style = Paint.Style.STROKE
        strokeWidth = 4f
        color = Theme.colorAccent
    }
    private val guidePaint = Paint(Paint.ANTI_ALIAS_FLAG).apply {
        style = Paint.Style.STROKE
        strokeWidth = 2f
        color = Color.argb(110, 255, 99, 91)  // faint version of the lead object's color
        pathEffect = DashPathEffect(floatArrayOf(10f, 10f), 0f)
    }
    private val listenerPaint = Paint(Paint.ANTI_ALIAS_FLAG).apply {
        style = Paint.Style.FILL
        color = Theme.colorTextSecondary
    }
    private val modePaint = Paint(Paint.ANTI_ALIAS_FLAG).apply {
        color = Theme.colorWarn
        textSize = 24f
    }
    private val statsPaint = Paint(Paint.ANTI_ALIAS_FLAG).apply {
        color = Theme.colorAccent
        textSize = 24f
    }
    private val trailPaint = Paint(Paint.ANTI_ALIAS_FLAG).apply {
        style = Paint.Style.STROKE
        strokeWidth = 4f
        color = Theme.colorTextPrimary
    }
    private val dropLinePaint = Paint(Paint.ANTI_ALIAS_FLAG).apply {
        style = Paint.Style.STROKE
        strokeWidth = 2f
        color = Theme.colorTextPrimary
    }

    private val cardRect = RectF()

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

        // Left: the 3D trail view, SQUARE rather than a wide rectangle - on
        // a widescreen TV, a wide-rectangle 3D panel left most of its own
        // width empty (the isometric projection's own natural bounding box
        // is close to square - see ISO_X_HALF_RANGE/ISO_Y_HALF_RANGE - so a
        // wider container just added dead margin, not more visible content).
        // Squaring it up, capped at half the screen width, both puts real
        // pixels back to use AND frees the rest of the width for the other
        // two panels - see hands-on feedback for why this was worth a
        // second layout pass after the first one merely narrowed them.
        // Vertically centered when height is the limiting dimension (a very
        // wide screen), so any leftover space splits evenly above/below
        // rather than collecting entirely at the bottom.
        val columnGap = Theme.spacingUnit
        val leftSize = minOf(height.toFloat(), width * 0.5f)
        val leftTop = (height - leftSize) / 2f
        val rightLeft = leftSize + columnGap
        val rightWidth = width - rightLeft
        // Top-down and elevation side by side, not stacked - each gets the
        // FULL row height this way instead of half of it (the "double
        // height" hands-on feedback asked for), splitting the width the
        // square 3D panel freed up between them instead.
        val panelGap = Theme.spacingUnit
        val sidePanelWidth = (rightWidth - panelGap) / 2f

        // Both readouts below used to float as independent canvas text at a
        // fixed screen position, clear of the cards at the time they were
        // added - but the first-launch/idle overlay cue (MainActivity's own
        // TextView, centered over this whole view) grew wider once it picked
        // up real typography, and started covering them. Folding each into
        // its own card's header row (see drawCard's `status` param) keeps
        // them inside a bounded, opaque card background instead of floating
        // text anything on top of this view can cover.
        val statsText = try {
            NativeBridge.nativeGetStreamStatsText()
        } catch (e: UnsatisfiedLinkError) {
            null
        }
        // Kept short deliberately ("D-PAD -> HEIGHT", not a full sentence):
        // this shares the top-down panel's header row with that panel's own
        // title, and that panel is the narrowest card in the layout.
        val modeText = inputController?.let { controller ->
            if (controller.axisMode == InputController.AxisMode.XY) "D-PAD → DEPTH" else "D-PAD → HEIGHT"
        }

        draw3DView(canvas, 0f, leftTop, leftSize, leftTop + leftSize, state, objectCount, leadFuture, statsText)

        drawPanel(
            canvas,
            left = rightLeft,
            top = 0f,
            right = rightLeft + sidePanelWidth,
            bottom = height.toFloat(),
            title = "Top-down (X/Y)",
            status = modeText,
            statusPaint = modePaint,
            state = state,
            objectCount = objectCount,
            horizontal = { i -> state[i * 4] },      // x
            // 1f - y, not y directly: drawPanel's own vertical-axis inversion
            // (below) puts a smaller value at the TOP of the panel - for the
            // elevation panel that's already right (ceiling should be up),
            // but for this one it means passing 1-y so the front wall
            // (y=0, where the listener faces - see InputController.kt's
            // matching D-pad-up-is-forward fix) renders at the top, not the
            // back wall. Paired with that fix so pressing D-pad up both
            // means "forward" AND visibly moves the dot up this panel,
            // rather than the two disagreeing with each other.
            vertical = { i -> 1f - state[i * 4 + 1] },

            verticalIsHeight = false,
            showTrajectoryGuide = true,
        )
        drawPanel(
            canvas,
            left = rightLeft + sidePanelWidth + panelGap,
            top = 0f,
            right = width.toFloat(),
            bottom = height.toFloat(),
            // "Side" dropped (was "Side elevation (X/Z)"): shorter title,
            // more headroom for whatever width this card ends up with.
            title = "Elevation (X/Z)",
            status = null,
            statusPaint = null,
            state = state,
            objectCount = objectCount,
            horizontal = { i -> state[i * 4] },           // x
            vertical = { i -> (state[i * 4 + 2] + 1f) / 2f }, // z in [-1,1] -> [0,1]
            verticalIsHeight = true,
            showTrajectoryGuide = false,
        )
    }

    /**
     * Fills+strokes a rounded-rect "card" background behind a panel, then
     * draws its uppercase caption and a divider separating the caption from
     * the plotted content below it - the one piece of chrome every panel in
     * this view shares, so [drawPanel] and [draw3DView] only differ in what
     * they draw INSIDE the card. Returns the content rect (inside the
     * caption/divider/padding) callers should plot into.
     */
    private fun drawCard(
        canvas: Canvas,
        left: Float,
        top: Float,
        right: Float,
        bottom: Float,
        title: String,
        status: String? = null,
        statusPaint: Paint? = null,
    ): RectF {
        cardRect.set(left, top, right, bottom)
        canvas.drawRoundRect(cardRect, Theme.cornerRadiusSmall, Theme.cornerRadiusSmall, cardBgPaint)
        canvas.drawRoundRect(cardRect, Theme.cornerRadiusSmall, Theme.cornerRadiusSmall, cardBorderPaint)

        val pad = 28f
        val titleBaseline = top + pad + 22f
        canvas.drawText(title.uppercase(), left + pad, titleBaseline, titlePaint)

        // A live readout (encode stats, D-pad axis mode) confined to this
        // card's own bounds - not floating independently over the whole
        // view, so nothing drawn on top of this View (the first-launch/idle
        // overlay cue) can ever cover it without also covering the card it
        // belongs to. On its OWN line below the title, right-aligned, rather
        // than sharing the title's row: the top-down panel is the narrowest
        // card in the layout and its title plus status text together don't
        // fit on one line without overlapping - confirmed on a real device
        // screenshot.
        var dividerY = titleBaseline + 20f
        if (status != null && statusPaint != null) {
            val statusBaseline = titleBaseline + 36f
            val statusWidth = statusPaint.measureText(status)
            canvas.drawText(status, right - pad - statusWidth, statusBaseline, statusPaint)
            dividerY = statusBaseline + 20f
        }
        canvas.drawLine(left + pad, dividerY, right - pad, dividerY, dividerPaint)

        return RectF(left + pad, dividerY + pad, right - pad, bottom - pad)
    }

    /**
     * Draws one object's dot with a soft halo beneath it and, if selected, a
     * ring around it - shared by [drawPanel] and [draw3DView] so both use
     * exactly the same visual language for "this is an object" /
     * "this is the one you're driving."
     */
    private fun drawObjectDot(canvas: Canvas, px: Float, py: Float, radius: Float, color: Int, isSelected: Boolean) {
        objectGlowPaint.color = color
        objectGlowPaint.alpha = 70
        canvas.drawCircle(px, py, radius * 2f, objectGlowPaint)
        objectPaint.color = color
        canvas.drawCircle(px, py, radius, objectPaint)
        if (isSelected) {
            canvas.drawCircle(px, py, radius + 10f, selectedRingPaint)
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
        statusText: String?,
    ) {
        val content = drawCard(canvas, left, top, right, bottom, "3D track", statusText, statsPaint)
        val panelLeft = content.left
        val panelTop = content.top
        val panelRight = content.right
        val panelBottom = content.bottom
        if (panelRight <= panelLeft || panelBottom <= panelTop) return

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
            drawObjectDot(canvas, px, py, 16f, objectColor(i), isSelected)
        }
    }

    private inline fun drawPanel(
        canvas: Canvas,
        left: Float,
        top: Float,
        right: Float,
        bottom: Float,
        title: String,
        status: String?,
        statusPaint: Paint?,
        state: FloatArray,
        objectCount: Int,
        horizontal: (Int) -> Float,
        vertical: (Int) -> Float,
        verticalIsHeight: Boolean,
        showTrajectoryGuide: Boolean,
    ) {
        val content = drawCard(canvas, left, top, right, bottom, title, status, statusPaint)
        if (content.right <= content.left || content.bottom <= content.top) return

        // The room itself is square in normalized coordinates (both x and y
        // range over the same [0,1] room-fraction scale - oamd.hpp's own
        // Position contract) - plotting it into a non-square content rect
        // would stretch one axis relative to the other and misrepresent
        // real distances (the trajectory guide circle would render as an
        // ellipse even though it's a genuine circle in room-space). Now that
        // top-down/elevation sit side by side rather than stacked, their
        // cards are routinely much wider than tall, so this can no longer be
        // left to "the card happens to be roughly square" the way it could
        // when panels were stacked - centre a true square plot inside
        // whatever rectangle the card actually gives.
        val availW = content.right - content.left
        val availH = content.bottom - content.top
        val roomSize = minOf(availW, availH)
        val roomLeft = content.left + (availW - roomSize) / 2f
        val roomTop = content.top + (availH - roomSize) / 2f
        val roomRight = roomLeft + roomSize
        val roomBottom = roomTop + roomSize

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

            drawObjectDot(canvas, px, py, radius, objectColor(i), isSelected)
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

        private fun objectColor(index: Int): Int = Theme.objectColors[index % Theme.objectColors.size]

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
