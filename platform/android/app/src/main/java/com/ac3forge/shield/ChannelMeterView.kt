package com.ac3forge.shield

import android.content.Context
import android.graphics.Canvas
import android.graphics.Color
import android.graphics.Paint
import android.graphics.RectF
import android.os.SystemClock
import android.util.AttributeSet
import android.view.Choreographer
import android.view.View

/**
 * The speaker-activity meter: six segmented-LED-style bars showing the RMS
 * level of each real bed channel the encode loop's last frame actually
 * produced (live_cursor.cpp's StreamStats::channel_levels, sourced from
 * AtmosEncoder::bed() - not a guess derived from room-position math, the
 * literal audio a legacy 5.1 decoder hears). Exists to visually connect
 * "here's the object's position" (the other panels) with "here's the trick
 * that lets an ordinary decoder still hear it" - watching the bed channels
 * shift as the object pans is a much more concrete way to show that off
 * than just asserting it.
 *
 * Styled after a real hardware VU meter, not a plain progress bar: discrete
 * lit/unlit segments (each individually rounded - small, uniform corner
 * radii avoid the pinched-arc distortion a single tall rounded rect got into
 * at low levels, see [drawChannel]'s own comment), a bottom-to-top color
 * ramp that runs hotter toward the top, and a slowly-decaying peak-hold
 * line riding above the current level - the same "catch the loudest recent
 * moment" behavior a real meter's peak indicator has, not just an
 * instantaneous readout.
 *
 * A fixed-height row, not overlaid on RoomView: this app already hit real
 * layout bugs once from overlaying text on views that had no reserved space
 * for it (see MainActivity's own history) - a proper sibling view in the
 * same LinearLayout avoids repeating that. Styled as the same rounded-card
 * chrome RoomView's own panels use (see [Theme]) so this reads as a fourth
 * panel in the same dashboard, not a separately-designed strip bolted on.
 */
class ChannelMeterView @JvmOverloads constructor(
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

    private val cardBgPaint = Paint(Paint.ANTI_ALIAS_FLAG).apply {
        style = Paint.Style.FILL
        color = Theme.colorSurface
    }
    private val cardBorderPaint = Paint(Paint.ANTI_ALIAS_FLAG).apply {
        style = Paint.Style.STROKE
        strokeWidth = 2f
        color = Theme.colorSurfaceBorder
    }
    private val titlePaint = Paint(Paint.ANTI_ALIAS_FLAG).apply {
        color = Theme.colorTextSecondary
        textSize = 22f
        letterSpacing = 0.08f
    }
    private val dividerPaint = Paint(Paint.ANTI_ALIAS_FLAG).apply {
        style = Paint.Style.STROKE
        strokeWidth = 2f
        color = Theme.colorSurfaceBorder
    }
    private val segmentPaint = Paint(Paint.ANTI_ALIAS_FLAG).apply {
        style = Paint.Style.FILL
    }
    private val peakPaint = Paint(Paint.ANTI_ALIAS_FLAG).apply {
        style = Paint.Style.FILL
        color = Theme.colorTextPrimary
    }
    private val labelPaint = Paint(Paint.ANTI_ALIAS_FLAG).apply {
        color = Theme.colorTextMuted
        textSize = 20f
        textAlign = Paint.Align.CENTER
    }

    private val cardRect = RectF()

    // Per-channel peak-hold state: the highest level seen recently, decaying
    // at PEAK_DECAY_PER_SEC toward the current level every frame rather than
    // just tracking the instantaneous level - see the class doc comment.
    private val peakLevels = FloatArray(CHANNEL_NAMES.size)
    private var lastDrawMs = 0L

    override fun onAttachedToWindow() {
        super.onAttachedToWindow()
        choreographer.postFrameCallback(frameCallback)
    }

    override fun onDetachedFromWindow() {
        choreographer.removeFrameCallback(frameCallback)
        super.onDetachedFromWindow()
    }

    override fun onMeasure(widthMeasureSpec: Int, heightMeasureSpec: Int) {
        super.onMeasure(widthMeasureSpec, MeasureSpec.makeMeasureSpec(HEIGHT_PX, MeasureSpec.EXACTLY))
    }

    override fun onDraw(canvas: Canvas) {
        super.onDraw(canvas)

        val levels = try {
            NativeBridge.nativeGetChannelLevels()
        } catch (e: UnsatisfiedLinkError) {
            return
        }
        if (levels.size < CHANNEL_NAMES.size) return

        val nowMs = SystemClock.elapsedRealtime()
        // Capped at 250ms: a big first-frame/resume gap would otherwise
        // read as "decay by an enormous amount," snapping every peak
        // straight to zero instead of falling smoothly.
        val dtS = if (lastDrawMs == 0L) 0f else ((nowMs - lastDrawMs) / 1000f).coerceIn(0f, 0.25f)
        lastDrawMs = nowMs

        val pad = 20f
        cardRect.set(0f, 0f, width.toFloat(), height.toFloat())
        canvas.drawRoundRect(cardRect, Theme.cornerRadiusSmall, Theme.cornerRadiusSmall, cardBgPaint)
        canvas.drawRoundRect(cardRect, Theme.cornerRadiusSmall, Theme.cornerRadiusSmall, cardBorderPaint)

        val titleBaseline = pad + 16f
        canvas.drawText("SPEAKER ACTIVITY (BED)", pad, titleBaseline, titlePaint)
        val dividerY = titleBaseline + 12f
        canvas.drawLine(pad, dividerY, width - pad, dividerY, dividerPaint)

        val labelSpace = 26f
        val barAreaTop = dividerY + 14f
        val barAreaBottom = height - labelSpace
        val barAreaHeight = barAreaBottom - barAreaTop
        val totalWidth = width - pad * 2f
        val barWidth = totalWidth / CHANNEL_NAMES.size * 0.55f
        val slot = totalWidth / CHANNEL_NAMES.size

        val gap = 3f
        // Clamped to the actual available height, not a fixed pixel value -
        // this is what keeps every segment (and its own corner radius, below)
        // well-formed regardless of exactly how tall this row ends up on a
        // given device, the same class of bug the old single-tall-rect
        // design got bitten by at low levels.
        val segHeight = ((barAreaHeight - gap * (SEGMENT_COUNT - 1)) / SEGMENT_COUNT).coerceAtLeast(1f)
        val segRadius = minOf(4f, segHeight / 2f)

        for (i in CHANNEL_NAMES.indices) {
            val centerX = pad + slot * i + slot / 2f
            val left = centerX - barWidth / 2f
            val right = centerX + barWidth / 2f

            val level = levels[i].coerceIn(0f, 1f)
            peakLevels[i] = maxOf(level, peakLevels[i] - PEAK_DECAY_PER_SEC * dtS)

            drawChannel(canvas, left, right, barAreaBottom, barAreaHeight, level, peakLevels[i], segHeight, segRadius, gap)
            canvas.drawText(CHANNEL_NAMES[i], centerX, height - 8f, labelPaint)
        }
    }

    /**
     * One channel's meter: SEGMENT_COUNT individually-rounded cells stacked
     * bottom-to-top (lit ones colored by [segmentColor], unlit ones the same
     * dim tone as every other card's own border/track color - no separate
     * "track" shape needed, the unlit segments already read as the rail),
     * plus a thin peak-hold line riding at `peak`'s height independent of
     * the segment grid. Each cell uses Canvas.drawRoundRect's own rx/ry
     * overload, which - unlike a hand-built Path.addRoundRect with a
     * per-corner radius array - self-clamps the radius against the rect's
     * own size, so there is no equivalent of the old single-bar design's
     * pinched/warped-corner bug at any level.
     */
    private fun drawChannel(
        canvas: Canvas,
        left: Float,
        right: Float,
        barAreaBottom: Float,
        barAreaHeight: Float,
        level: Float,
        peak: Float,
        segHeight: Float,
        segRadius: Float,
        gap: Float,
    ) {
        val litSegments = (level * SEGMENT_COUNT).toInt().coerceIn(0, SEGMENT_COUNT)
        for (s in 0 until SEGMENT_COUNT) {
            val segBottom = barAreaBottom - s * (segHeight + gap)
            val segTop = segBottom - segHeight
            val fraction = (s + 1).toFloat() / SEGMENT_COUNT
            segmentPaint.color = if (s < litSegments) segmentColor(fraction) else Theme.colorSurfaceBorder
            canvas.drawRoundRect(left, segTop, right, segBottom, segRadius, segRadius, segmentPaint)
        }

        if (peak > 0.015f) {
            val peakY = barAreaBottom - barAreaHeight * peak
            canvas.drawRoundRect(left, peakY - 3f, right, peakY + 3f, 3f, 3f, peakPaint)
        }
    }

    companion object {
        // AC-3 coded order - matches live_cursor.cpp's
        // StreamStats::channel_levels / AtmosEncoder::bed().
        private val CHANNEL_NAMES = arrayOf("L", "C", "R", "Ls", "Rs", "LFE")
        // Deliberately compact - this panel is "interesting, but doesn't
        // need to be prominent" per hands-on feedback: the 3D track is the
        // panel worth spending screen space on, this one just needs to stay
        // legible at a glance. SEGMENT_COUNT drops with it so each remaining
        // segment stays a reasonable visual size rather than becoming a
        // hairline.
        private const val HEIGHT_PX = 132
        private const val SEGMENT_COUNT = 6
        // Full-scale-to-zero in a bit under a second - fast enough that a
        // held peak still reads as "the recent loudest moment," not a
        // several-second-old, stale-looking readout.
        private const val PEAK_DECAY_PER_SEC = 1.1f

        // Bottom-to-top color ramp: dim accent -> full accent -> warm
        // "hot" tone for the top ~35% of the scale - the same warm hue the
        // D-pad axis-mode readout uses elsewhere in this app, so "hot" reads
        // as a consistent color meaning across the whole dashboard, not a
        // one-off red/yellow VU-meter convention that clashes with
        // everything else on screen.
        private fun segmentColor(fraction: Float): Int = if (fraction <= 0.65f) {
            lerpColor(Theme.colorAccentDim, Theme.colorAccent, fraction / 0.65f)
        } else {
            lerpColor(Theme.colorAccent, Theme.colorWarn, (fraction - 0.65f) / 0.35f)
        }

        private fun lerpColor(from: Int, to: Int, t: Float): Int {
            val tt = t.coerceIn(0f, 1f)
            return Color.rgb(
                (Color.red(from) + (Color.red(to) - Color.red(from)) * tt).toInt(),
                (Color.green(from) + (Color.green(to) - Color.green(from)) * tt).toInt(),
                (Color.blue(from) + (Color.blue(to) - Color.blue(from)) * tt).toInt(),
            )
        }
    }
}
