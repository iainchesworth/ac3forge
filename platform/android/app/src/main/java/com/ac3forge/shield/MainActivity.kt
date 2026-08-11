package com.ac3forge.shield

import android.app.Activity
import android.graphics.Typeface
import android.graphics.drawable.GradientDrawable
import android.os.Bundle
import android.os.Handler
import android.os.Looper
import android.os.SystemClock
import android.text.Spannable
import android.text.SpannableString
import android.text.style.AbsoluteSizeSpan
import android.text.style.ForegroundColorSpan
import android.text.style.StyleSpan
import android.util.Log
import android.view.Gravity
import android.view.KeyEvent
import android.view.MotionEvent
import android.view.View
import android.widget.FrameLayout
import android.widget.LinearLayout
import android.widget.TextView
import kotlin.concurrent.thread

private const val TAG = "ShieldAtmosDemo"

// How long the first-launch orientation cue stays up before auto-dismissing
// if nobody touches a control - long enough to read twice, short enough not
// to feel stuck. Dismissed immediately on first real input either way (see
// MainActivity.onUserInputActivity).
private const val ORIENTATION_CUE_MS = 5000L

// How long with no input before the idle/attract prompt appears - a demo
// left alone (between visitors at a booth, say) should invite the next
// person rather than just sit there having already made its point.
private const val IDLE_PROMPT_MS = 14000L
private const val IDLE_CHECK_INTERVAL_MS = 2000L

// How long the overlay cue's fade in/out takes - fast enough not to feel
// laggy, slow enough to read as an intentional transition rather than a
// jarring pop.
private const val OVERLAY_FADE_MS = 220L

/**
 * Loads the native library, registers the [PassthroughBridge], runs the
 * HDMI capability probe, starts the encode loop (live_cursor.cpp), wires
 * Shield Controller/remote input through [InputController], and shows the
 * live object positions via [RoomView] - the startup/capability report stays
 * as a one-shot logcat entry plus a small on-screen control-hints overlay
 * rather than replacing the room view, since it doesn't change frame to
 * frame the way the room view does.
 *
 * Visual chrome (title/hints bars, the overlay cue) is built by hand here
 * rather than from an XML layout/theme - see [Theme]'s own comment for why
 * one shared palette file exists to keep it all consistent with RoomView's
 * and ChannelMeterView's own Canvas-drawn cards.
 *
 * Diagnostic mode: `am start ... --es play_file /sdcard/Download/whatever.ec3`
 * skips the live cursor/object demo entirely and instead streams that real,
 * already-encoded file through file_replay.cpp's PassthroughSink path - see
 * NativeBridge.nativePlayEac3File's doc comment for why this exists.
 */
class MainActivity : Activity() {
    private val passthroughBridge = PassthroughBridge()
    private val inputController = InputController()
    private val mainHandler = Handler(Looper.getMainLooper())

    // The single overlay banner shared by the first-launch orientation cue
    // and the idle/attract prompt (items 3/5 of the post-feedback demo
    // punch list) - the two are mutually exclusive by construction
    // (orientationCueShowing gates the idle checker below), so one TextView
    // is enough rather than two views fighting over the same screen space.
    private lateinit var overlayCue: TextView
    private var orientationCueShowing = false
    private var lastInputAtMs = 0L

    private val orientationCueTimeout = Runnable { hideOrientationCue() }

    // Polls rather than reacts to "input stopped" (there is no such event -
    // see InputController's own comment on why release-to-decay works the
    // same way): checks elapsed idle time on a slow, cheap timer and
    // shows/hides the attract prompt accordingly. Reposts itself for as long
    // as the Activity is resumed (started in onResume, cancelled in
    // onPause).
    private val idleChecker = object : Runnable {
        override fun run() {
            // Diagnostic-mode (play_file) returns from onCreate before
            // overlayCue is ever built - guard rather than crash, since
            // onResume/onPause still run normally in that mode too.
            if (!::overlayCue.isInitialized) return
            if (!orientationCueShowing) {
                val idleMs = SystemClock.elapsedRealtime() - lastInputAtMs
                if (idleMs >= IDLE_PROMPT_MS && overlayCue.visibility != View.VISIBLE) {
                    setOverlayCueText("Press any button to take control")
                    showOverlayCue()
                } else if (idleMs < IDLE_PROMPT_MS && overlayCue.visibility == View.VISIBLE) {
                    hideOverlayCue()
                }
            }
            mainHandler.postDelayed(this, IDLE_CHECK_INTERVAL_MS)
        }
    }

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

        // Must run before nativeStartLiveCursor - the encode loop loads the
        // bundled lead-voice sample once at startup, on its own thread; a
        // late call would just miss it (see NativeBridge.nativeSetAssetManager's
        // own doc comment - missing this is a graceful fallback, not a crash).
        try {
            NativeBridge.nativeSetAssetManager(assets)
        } catch (e: UnsatisfiedLinkError) {
            Log.e(TAG, "nativeSetAssetManager failed - lead object will use its live-synthesized voice", e)
        }

        val liveCursorStarted = try {
            NativeBridge.nativeStartLiveCursor()
        } catch (e: UnsatisfiedLinkError) {
            Log.e(TAG, "encode loop failed to start", e)
            false
        }
        Log.i(TAG, "encode loop started: $liveCursorStarted")

        val titleBar = buildTitleBar()
        val roomView = RoomView(this@MainActivity, inputController)
        val channelMeter = ChannelMeterView(this@MainActivity)
        val hintsBar = buildHintsBar(channelMeter)

        // overlayCue sits ON TOP of roomView only (a FrameLayout scoped to
        // just this one row), not overlaid across the whole screen the way
        // the old title/hints bug did - it never competes for space with
        // channelMeter/hints below, only with roomView's own content, and
        // roomView keeps drawing normally underneath it since this is a
        // transient banner, not permanently-reserved chrome.
        overlayCue = TextView(this).apply {
            textSize = 26f
            gravity = Gravity.CENTER
            setTextColor(Theme.colorTextPrimary)
            background = GradientDrawable().apply {
                cornerRadius = Theme.cornerRadiusLarge
                setColor(Theme.colorSurface)
                setStroke(3, Theme.colorAccent)
            }
            setPadding(56, 40, 56, 40)
            alpha = 0f
            visibility = View.GONE
            // RoomView's own left ("3D track") column is now a square capped
            // at roughly half this row's width (see its own leftSize split,
            // narrowed considerably from its original ~70-75% share once
            // top-down/elevation moved beside it rather than being squeezed
            // into a narrow stacked column) - capped comfortably under that
            // so this cue, centered across the WHOLE row, can never grow
            // wide enough to visually reach into the top-down/elevation
            // cards on the right, regardless of device aspect ratio or how
            // long a future cue's text gets.
            maxWidth = (resources.displayMetrics.widthPixels * 0.42f).toInt()
        }
        val roomStack = FrameLayout(this).apply {
            addView(
                roomView,
                FrameLayout.LayoutParams(
                    FrameLayout.LayoutParams.MATCH_PARENT,
                    FrameLayout.LayoutParams.MATCH_PARENT,
                ),
            )
            addView(
                overlayCue,
                FrameLayout.LayoutParams(
                    FrameLayout.LayoutParams.WRAP_CONTENT,
                    FrameLayout.LayoutParams.WRAP_CONTENT,
                    Gravity.CENTER,
                ),
            )
        }

        // Title/hints bars run edge-to-edge (real AV-receiver chrome does
        // too), but the room content sits inset from the screen edges on a
        // plain dark background - contentColumn owns that padding so
        // RoomView's own rounded cards read as panels floating on the
        // background rather than touching the bezel. ChannelMeterView no
        // longer lives in this column at all - see buildHintsBar.
        val pad = Theme.spacingUnit.toInt()
        val contentColumn = LinearLayout(this).apply {
            orientation = LinearLayout.VERTICAL
            setPadding(pad, pad, pad, pad)
            addView(
                roomStack,
                LinearLayout.LayoutParams(LinearLayout.LayoutParams.MATCH_PARENT, LinearLayout.LayoutParams.MATCH_PARENT),
            )
        }

        val root = LinearLayout(this).apply {
            orientation = LinearLayout.VERTICAL
            setBackgroundColor(Theme.colorBackground)
            addView(titleBar, LinearLayout.LayoutParams(
                LinearLayout.LayoutParams.MATCH_PARENT, LinearLayout.LayoutParams.WRAP_CONTENT,
            ))
            addView(contentColumn, LinearLayout.LayoutParams(
                LinearLayout.LayoutParams.MATCH_PARENT, 0, 1f,
            ))
            addView(hintsBar, LinearLayout.LayoutParams(
                LinearLayout.LayoutParams.MATCH_PARENT, LinearLayout.LayoutParams.WRAP_CONTENT,
            ))
        }
        setContentView(root)

        inputController.onInputActivity = { onUserInputActivity() }
        lastInputAtMs = SystemClock.elapsedRealtime()
        setOverlayCueText(
            "This is the front wall",
            "Up on the stick/D-pad = toward the screen",
        )
        orientationCueShowing = true
        showOverlayCue()
        mainHandler.postDelayed(orientationCueTimeout, ORIENTATION_CUE_MS)
    }

    // Edge-to-edge top bar: a bold primary line plus a small accent-colored
    // caption underneath (the kind of two-tier title a real AVR's own front
    // panel display uses), separated from the content below it by a hairline
    // divider rather than relying on a flat color change alone to read as
    // "chrome, not content."
    private fun buildTitleBar(): View = LinearLayout(this).apply {
        orientation = LinearLayout.VERTICAL
        setBackgroundColor(Theme.colorSurface)
        addView(TextView(this@MainActivity).apply {
            text = "ac3forge — Shield Atmos Demo"
            textSize = 24f
            typeface = Typeface.DEFAULT_BOLD
            setTextColor(Theme.colorTextPrimary)
            gravity = Gravity.CENTER_HORIZONTAL
            setPadding(24, 20, 24, 2)
        })
        addView(TextView(this@MainActivity).apply {
            text = "LIVE DOLBY ATMOS OBJECT DEMO"
            textSize = 14f
            letterSpacing = 0.14f
            setTextColor(Theme.colorAccent)
            gravity = Gravity.CENTER_HORIZONTAL
            setPadding(24, 0, 24, 16)
        })
        addView(dividerView(), LinearLayout.LayoutParams(LinearLayout.LayoutParams.MATCH_PARENT, 2))
    }

    // Edge-to-edge bottom bar: the speaker-activity meter moved down here,
    // beside a shrunk control-hints column, rather than its own full-width
    // row above this one - per hands-on feedback, that row's whole purpose
    // was to give the 3D track (and now the side-by-side top-down/elevation
    // panels) back the vertical space the meter used to take, while keeping
    // the meter itself visible, just smaller and to the side.
    private fun buildHintsBar(channelMeter: View): View = LinearLayout(this).apply {
        orientation = LinearLayout.VERTICAL
        setBackgroundColor(Theme.colorSurface)
        addView(dividerView(), LinearLayout.LayoutParams(LinearLayout.LayoutParams.MATCH_PARENT, 2))
        addView(
            LinearLayout(this@MainActivity).apply {
                orientation = LinearLayout.HORIZONTAL
                gravity = Gravity.CENTER_VERTICAL
                addView(
                    channelMeter,
                    LinearLayout.LayoutParams(0, LinearLayout.LayoutParams.WRAP_CONTENT, 2f),
                )
                addView(TextView(this@MainActivity).apply {
                    text = "Stick/D-pad: push the lead object off its course, it drifts back " +
                        "when you let go   •   Right stick / L1+R1: height   •   Press A/center: " +
                        "D-pad up/down toggles between depth and height   •   Pause: isolate the " +
                        "lead   •   Play: bring the ambient tones back\n" +
                        "● lead (yours to push around)   ● ● two ambient tones, always on their " +
                        "own course"
                    textSize = 12f
                    gravity = Gravity.CENTER
                    setTextColor(Theme.colorTextSecondary)
                    setLineSpacing(5f, 1f)
                    setPadding(20, 14, 24, 14)
                }, LinearLayout.LayoutParams(0, LinearLayout.LayoutParams.WRAP_CONTENT, 3f))
            },
            LinearLayout.LayoutParams(LinearLayout.LayoutParams.MATCH_PARENT, LinearLayout.LayoutParams.WRAP_CONTENT),
        )
    }

    private fun dividerView(): View = View(this).apply {
        setBackgroundColor(Theme.colorSurfaceBorder)
    }

    // Builds overlayCue's text as a two-tier Spannable (a larger bold
    // headline, an optional smaller dimmer subtitle) rather than plain text
    // - matches the title bar's own headline/caption pairing so the cue
    // reads as part of the same design language, not a plain system toast.
    private fun setOverlayCueText(headline: String, subtitle: String? = null) {
        val full = if (subtitle != null) "$headline\n$subtitle" else headline
        val spannable = SpannableString(full)
        spannable.setSpan(StyleSpan(Typeface.BOLD), 0, headline.length, Spannable.SPAN_EXCLUSIVE_EXCLUSIVE)
        spannable.setSpan(AbsoluteSizeSpan(30, true), 0, headline.length, Spannable.SPAN_EXCLUSIVE_EXCLUSIVE)
        if (subtitle != null) {
            val start = headline.length + 1
            spannable.setSpan(ForegroundColorSpan(Theme.colorTextSecondary), start, full.length, Spannable.SPAN_EXCLUSIVE_EXCLUSIVE)
            spannable.setSpan(AbsoluteSizeSpan(22, true), start, full.length, Spannable.SPAN_EXCLUSIVE_EXCLUSIVE)
        }
        overlayCue.text = spannable
    }

    private fun showOverlayCue() {
        overlayCue.visibility = View.VISIBLE
        overlayCue.animate().cancel()
        overlayCue.animate().alpha(1f).setDuration(OVERLAY_FADE_MS).start()
    }

    private fun hideOverlayCue() {
        overlayCue.animate().cancel()
        overlayCue.animate().alpha(0f).setDuration(OVERLAY_FADE_MS).withEndAction {
            overlayCue.visibility = View.GONE
        }.start()
    }

    // Dismisses whichever of the two overlayCue uses is currently up (first
    // real input always wins over the auto-dismiss timer) and resets the
    // idle clock the attract prompt watches. Called from
    // InputController.onInputActivity - see that field's own comment for why
    // this class, not InputController, owns the actual UI reaction.
    private fun onUserInputActivity() {
        lastInputAtMs = SystemClock.elapsedRealtime()
        if (orientationCueShowing) {
            hideOrientationCue()
        } else if (overlayCue.visibility == View.VISIBLE) {
            hideOverlayCue()
        }
    }

    private fun hideOrientationCue() {
        orientationCueShowing = false
        hideOverlayCue()
        mainHandler.removeCallbacks(orientationCueTimeout)
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
            setTextColor(Theme.colorTextPrimary)
            setBackgroundColor(Theme.colorBackground)
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
        lastInputAtMs = SystemClock.elapsedRealtime()
        mainHandler.postDelayed(idleChecker, IDLE_CHECK_INTERVAL_MS)
    }

    override fun onPause() {
        inputController.stop()
        mainHandler.removeCallbacks(idleChecker)
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
