package com.ac3forge.shield

import android.app.Activity
import android.content.pm.PackageManager
import android.graphics.Typeface
import android.os.Bundle
import android.util.Log
import android.view.Gravity
import android.widget.ImageView
import android.widget.LinearLayout
import android.widget.ScrollView
import android.widget.TextView

private const val TAG = "ShieldAtmosDemo"

/**
 * A dedicated screen rather than a dialog: MainActivity builds its whole UI
 * by hand (plain [android.widget.View]s, no XML layouts/menu chrome - see
 * its own class-header comment), and this app is entirely D-pad/remote
 * driven with no window-manager affordance for an overlay dialog to sit
 * inside the way a mouse-driven app would use one. Launched from
 * MainActivity's `KEYCODE_INFO` handler (the TV remote's "info" button);
 * finishes on the ordinary BACK key, the default [Activity] behaviour - no
 * custom handling needed here.
 */
class AboutActivity : Activity() {
    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)

        // PackageManager, not BuildConfig: this project doesn't set
        // android.buildFeatures.buildConfig = true (off by default since
        // AGP 8.0), and turning it on just for this one screen's two fields
        // isn't worth the extra generated-sources build step. This also
        // reads the actually-installed APK's version rather than a
        // compile-time constant, which is the more honest source anyway.
        @Suppress("DEPRECATION") // versionCode: no minSdk-26-safe replacement (longVersionCode needs API 28)
        val (appVersionName, appVersionCode) = try {
            val info = packageManager.getPackageInfo(packageName, 0)
            info.versionName to info.versionCode
        } catch (e: PackageManager.NameNotFoundException) {
            "(unknown)" to 0
        }

        val version = try {
            NativeBridge.nativeVersionString()
        } catch (e: UnsatisfiedLinkError) {
            Log.e(TAG, "native library failed to load/link", e)
            "(native link failed - see logcat)"
        }

        val pad = Theme.spacingUnit.toInt()
        val density = resources.displayMetrics.density
        val content = LinearLayout(this).apply {
            orientation = LinearLayout.VERTICAL
            gravity = Gravity.CENTER_HORIZONTAL
            setPadding(pad * 2, pad * 2, pad * 2, pad * 2)

            addView(
                ImageView(this@AboutActivity).apply {
                    // The same TV-launcher banner (see AndroidManifest.xml's
                    // android:banner) rather than a separate About-specific
                    // image - one asset, one place its design lives.
                    setImageResource(R.drawable.banner)
                    adjustViewBounds = true
                },
                LinearLayout.LayoutParams((320 * density).toInt(), (180 * density).toInt()).apply {
                    bottomMargin = pad
                },
            )

            addView(TextView(this@AboutActivity).apply {
                text = "ac3forge — Shield Atmos Demo"
                textSize = 24f
                typeface = Typeface.DEFAULT_BOLD
                setTextColor(Theme.colorTextPrimary)
                gravity = Gravity.CENTER_HORIZONTAL
                setPadding(0, 0, 0, pad)
            })

            addView(kicker("VERSION"))
            addView(body(
                "App: $appVersionName (build $appVersionCode)\n" +
                    "ac3::forge (native): $version",
            ))

            addView(kicker("LICENSE"))
            addView(body(
                "ac3forge is free software: you can redistribute it and/or modify it " +
                    "under the terms of the GNU General Public License as published by " +
                    "the Free Software Foundation, either version 3 of the License, or " +
                    "(at your option) any later version. It is distributed WITHOUT ANY " +
                    "WARRANTY; see https://www.gnu.org/licenses/gpl-3.0.html for details.",
            ))

            addView(TextView(this@AboutActivity).apply {
                text = "Press BACK to return."
                textSize = 13f
                setTextColor(Theme.colorTextMuted)
                gravity = Gravity.CENTER_HORIZONTAL
                setPadding(0, pad, 0, 0)
            })
        }

        setContentView(
            ScrollView(this).apply {
                setBackgroundColor(Theme.colorBackground)
                addView(content)
            },
        )
    }

    private fun kicker(text: String): TextView = TextView(this).apply {
        this.text = text
        textSize = 12f
        letterSpacing = 0.12f
        setTextColor(Theme.colorTextMuted)
        setPadding(0, Theme.spacingUnit.toInt(), 0, 8)
    }

    private fun body(text: String): TextView = TextView(this).apply {
        this.text = text
        textSize = 15f
        setTextColor(Theme.colorTextSecondary)
        setLineSpacing(6f, 1f)
        gravity = Gravity.CENTER_HORIZONTAL
        setPadding(0, 0, 0, Theme.spacingUnit.toInt())
    }
}
