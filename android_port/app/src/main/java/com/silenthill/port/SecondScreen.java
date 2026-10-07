package com.silenthill.port;

import android.app.Activity;
import android.app.Presentation;
import android.content.Context;
import android.graphics.Canvas;
import android.graphics.Color;
import android.graphics.Paint;
import android.graphics.Path;
import android.graphics.RectF;
import android.graphics.Typeface;
import android.hardware.display.DisplayManager;
import android.os.Build;
import android.os.Bundle;
import android.os.Handler;
import android.os.Looper;
import android.util.DisplayMetrics;
import android.util.Log;
import android.view.Display;
import android.view.KeyEvent;
import android.view.MotionEvent;
import android.view.View;
import android.view.Window;
import android.view.WindowInsets;
import android.view.WindowInsetsController;
import android.view.WindowManager;

import java.io.File;
import java.io.FileInputStream;
import java.io.FileOutputStream;
import java.io.InputStream;
import java.io.OutputStream;
import java.nio.charset.Charset;
import java.util.ArrayList;
import java.util.List;
import java.util.Locale;
import java.util.Properties;

/**
 * The inventory on a second display (AYN Thor and other dual-screen handhelds).
 *
 * The game itself is untouched: it keeps its one SDL window on the display the
 * activity lives on. This puts an ordinary Android window on the OTHER display
 * and draws the inventory there from a snapshot the game thread publishes
 * (pc_second_screen.c). Phase 1 is read-only.
 *
 * The window is a Presentation, and it is deliberately NOT focusable. Android
 * delivers key and gamepad events to the focused window of the focused
 * display, so a focusable window down there would take the pad away from the
 * game the first time it was shown or touched. Touches still arrive on a
 * non-focusable window. This is the same arrangement the 3DS emulators use for
 * their second screen on these devices.
 */
final class SecondScreen implements DisplayManager.DisplayListener {

    static final String TAG = "SH2Screen";

    private static final String CFG_NAME = "second_screen.cfg";
    private static final int POLL_MS = 100;

    private final Activity activity;
    private final DisplayManager displayManager;
    private final Handler handler = new Handler(Looper.getMainLooper());

    private boolean enabled = true;
    private int wantedDisplayId = -1;
    private boolean focusable = false;
    private boolean debugOverlay = false;
    private float textScale = 1.0f;

    private Panel panel;
    private boolean started;
    private boolean listening;

    private static native byte[] nativePoll(int lastSerial);

    SecondScreen(Activity activity) {
        this.activity = activity;
        this.displayManager = (DisplayManager) activity.getSystemService(Context.DISPLAY_SERVICE);
        loadConfig();
    }

    /* ------------------------------------------------------------------ */
    /* Lifecycle, driven from SilentHillActivity.                          */
    /* ------------------------------------------------------------------ */

    void start() {
        started = true;
        if (!enabled) {
            Log.i(TAG, "disabled in " + CFG_NAME);
            return;
        }
        if (!listening) {
            displayManager.registerDisplayListener(this, handler);
            listening = true;
        }
        logDisplays();
        refresh();
    }

    void stop() {
        started = false;
        if (listening) {
            displayManager.unregisterDisplayListener(this);
            listening = false;
        }
        dismiss();
    }

    @Override public void onDisplayAdded(int displayId) { refresh(); }
    @Override public void onDisplayRemoved(int displayId) { refresh(); }

    @Override public void onDisplayChanged(int displayId) {
        /* Fires for rotation and brightness too. Only a display going away, or
         * one appearing while nothing is shown, needs a new window. */
        if (panel == null || !panel.getDisplay().isValid()) {
            refresh();
        }
    }

    private void refresh() {
        if (!started || !enabled || activity.isFinishing() || activity.isDestroyed()) {
            return;
        }

        Display target = pickDisplay();

        if (panel != null && target != null
                && panel.getDisplay().getDisplayId() == target.getDisplayId()
                && panel.getDisplay().isValid()) {
            return;
        }

        dismiss();

        if (target == null) {
            Log.i(TAG, "no secondary display to use");
            return;
        }

        try {
            panel = new Panel(activity, target);
            panel.show();
            Log.i(TAG, "showing on display " + target.getDisplayId() + " '" + target.getName() + "'");
        } catch (WindowManager.InvalidDisplayException e) {
            /* The display went away between the pick and the show. The
             * listener fires again when it settles. */
            Log.w(TAG, "display vanished before show: " + e.getMessage());
            panel = null;
        } catch (RuntimeException e) {
            Log.e(TAG, "could not show on display " + target.getDisplayId(), e);
            panel = null;
        }
    }

    private void dismiss() {
        if (panel != null) {
            try {
                panel.dismiss();
            } catch (RuntimeException e) {
                Log.w(TAG, "dismiss: " + e.getMessage());
            }
            panel = null;
        }
    }

    /* ------------------------------------------------------------------ */
    /* Which display.                                                      */
    /* ------------------------------------------------------------------ */

    private int ownDisplayId() {
        Display d = null;
        if (Build.VERSION.SDK_INT >= 30) {
            try {
                d = activity.getDisplay();
            } catch (RuntimeException ignored) {
                d = null;
            }
        }
        if (d == null) {
            d = activity.getWindowManager().getDefaultDisplay();
        }
        return (d != null) ? d.getDisplayId() : Display.DEFAULT_DISPLAY;
    }

    private Display pickDisplay() {
        int own = ownDisplayId();
        Display[] all = displayManager.getDisplays();
        List<Display> usable = new ArrayList<Display>();
        String defaultName = null;

        for (Display d : all) {
            if (d != null && d.getDisplayId() == Display.DEFAULT_DISPLAY) {
                defaultName = d.getName();
            }
        }

        for (Display d : all) {
            if (d == null || !d.isValid()) continue;
            if (d.getDisplayId() == own) continue;
            if (d.getState() == Display.STATE_OFF) continue;
            usable.add(d);
        }

        if (usable.isEmpty()) {
            return null;
        }

        if (wantedDisplayId >= 0) {
            for (Display d : usable) {
                if (d.getDisplayId() == wantedDisplayId) return d;
            }
            Log.w(TAG, "display=" + wantedDisplayId + " from " + CFG_NAME + " is not available; picking automatically");
        }

        /* Some handhelds keep a permanent virtual display carrying the default
         * display's own name, which is never the panel anyone means. Prefer one
         * that is named differently, and fall back to the first otherwise. */
        for (Display d : usable) {
            String n = d.getName();
            if (n != null && !n.equals(defaultName) && !n.toLowerCase(Locale.ROOT).contains("built")) {
                return d;
            }
        }
        return usable.get(0);
    }

    private void logDisplays() {
        int own = ownDisplayId();
        for (Display d : displayManager.getDisplays()) {
            if (d == null) continue;
            DisplayMetrics m = new DisplayMetrics();
            d.getRealMetrics(m);
            Log.i(TAG, "display id=" + d.getDisplayId()
                    + " name='" + d.getName() + "'"
                    + " " + m.widthPixels + "x" + m.heightPixels
                    + " dpi=" + m.densityDpi
                    + " flags=0x" + Integer.toHexString(d.getFlags())
                    + " state=" + d.getState()
                    + " valid=" + d.isValid()
                    + (d.getDisplayId() == own ? " <- game" : ""));
        }
    }

    /* ------------------------------------------------------------------ */
    /* second_screen.cfg, next to config.cfg.                              */
    /* ------------------------------------------------------------------ */

    private void loadConfig() {
        File root = StorageLocations.resolve(activity);
        if (root == null) {
            return;
        }
        File f = new File(root, CFG_NAME);

        if (!f.isFile()) {
            writeDefaultConfig(f);
            return;
        }

        Properties p = new Properties();
        InputStream in = null;
        try {
            in = new FileInputStream(f);
            p.load(in);
        } catch (Exception e) {
            Log.w(TAG, "cannot read " + f + ": " + e.getMessage());
            return;
        } finally {
            if (in != null) {
                try { in.close(); } catch (Exception ignored) { }
            }
        }

        enabled = !"0".equals(p.getProperty("enabled", "1").trim());
        focusable = "1".equals(p.getProperty("focusable", "0").trim());
        debugOverlay = "1".equals(p.getProperty("debug", "0").trim());

        String disp = p.getProperty("display", "auto").trim();
        if (!"auto".equalsIgnoreCase(disp)) {
            try {
                wantedDisplayId = Integer.parseInt(disp);
            } catch (NumberFormatException e) {
                Log.w(TAG, "display=" + disp + " is not a number or 'auto'");
            }
        }

        try {
            float s = Float.parseFloat(p.getProperty("text_scale", "1.0").trim());
            if (s >= 0.5f && s <= 2.0f) {
                textScale = s;
            }
        } catch (NumberFormatException e) {
            Log.w(TAG, "text_scale is not a number");
        }

        Log.i(TAG, "config: enabled=" + enabled + " display=" + disp + " focusable=" + focusable
                + " debug=" + debugOverlay + " text_scale=" + textScale);
    }

    private void writeDefaultConfig(File f) {
        String text =
              "# Second-screen inventory (dual-display handhelds such as the AYN Thor).\n"
            + "# Edit and restart the game. Delete this file to get the defaults back.\n"
            + "\n"
            + "# 1 = show the inventory on the second display, 0 = off.\n"
            + "enabled=1\n"
            + "\n"
            + "# Which display. 'auto' picks one, or put a display id from the log\n"
            + "# (adb logcat -s SH2Screen) if it picks the wrong one.\n"
            + "display=auto\n"
            + "\n"
            + "# 0 = the second screen never takes input focus (recommended: the pad\n"
            + "#     always stays with the game).\n"
            + "# 1 = it may take focus. Only try this if system bars or buttons stay\n"
            + "#     visible on the second screen with 0.\n"
            + "focusable=0\n"
            + "\n"
            + "# Text size multiplier, 0.5 to 2.0.\n"
            + "text_scale=1.0\n"
            + "\n"
            + "# 1 = draw the window size and system-bar insets on the second screen,\n"
            + "#     for a screenshot when reporting a layout problem.\n"
            + "debug=0\n";
        OutputStream out = null;
        try {
            out = new FileOutputStream(f);
            out.write(text.getBytes(Charset.forName("UTF-8")));
        } catch (Exception e) {
            Log.w(TAG, "cannot write " + f + ": " + e.getMessage());
        } finally {
            if (out != null) {
                try { out.close(); } catch (Exception ignored) { }
            }
        }
    }

    /* ------------------------------------------------------------------ */
    /* Snapshot. Layout mirrors Ss_Build in pc_second_screen.c.            */
    /* ------------------------------------------------------------------ */

    static final class Item {
        int id;
        int count;
        int slot;
        String name;
    }

    static final class Snapshot {
        int serial;
        boolean session;
        boolean flashlightOn;
        boolean radioOn;
        int gameState;
        int healthQ12;
        int equippedId;
        int ammoLoaded;
        int ammoReserve;
        int equippedSlot;
        final List<Item> items = new ArrayList<Item>();
    }

    private static final Charset LATIN1 = Charset.forName("ISO-8859-1");

    static Snapshot parse(byte[] b) {
        final int head = 4 + 20;
        if (b == null || b.length < head) return null;
        if (b[4] != 'S' || b[5] != 'H' || b[6] != '2' || b[7] != 'S' || b[8] != 1) return null;

        Snapshot s = new Snapshot();
        s.serial = (b[0] & 0xFF) | ((b[1] & 0xFF) << 8) | ((b[2] & 0xFF) << 16) | ((b[3] & 0xFF) << 24);

        int flags = b[9] & 0xFF;
        s.session = (flags & 1) != 0;
        s.flashlightOn = (flags & 2) != 0;
        s.radioOn = (flags & 4) != 0;
        s.gameState = b[10];
        s.healthQ12 = (b[12] & 0xFF) | ((b[13] & 0xFF) << 8) | ((b[14] & 0xFF) << 16) | ((b[15] & 0xFF) << 24);
        s.equippedId = b[16] & 0xFF;
        s.ammoLoaded = b[17] & 0xFF;
        s.ammoReserve = b[18] & 0xFF;
        int count = b[19] & 0xFF;
        s.equippedSlot = b[21] & 0xFF;

        int p = head;
        for (int i = 0; i < count; i++) {
            if (p + 4 > b.length) break;
            Item it = new Item();
            it.id = b[p] & 0xFF;
            it.count = b[p + 1] & 0xFF;
            it.slot = b[p + 2] & 0xFF;
            int len = b[p + 3] & 0xFF;
            if (p + 4 + len > b.length) break;
            it.name = cleanName(new String(b, p + 4, len, LATIN1));
            s.items.add(it);
            p += 4 + len;
        }
        return s;
    }

    /** The game's text dialect: '_' is a space, and names can carry a newline. */
    static String cleanName(String raw) {
        StringBuilder sb = new StringBuilder(raw.length());
        boolean space = false;
        for (int i = 0; i < raw.length(); i++) {
            char c = raw.charAt(i);
            if (c == '_' || c == '\n' || c == '\t' || c == ' ' || c == '\r') {
                space = sb.length() > 0;
                continue;
            }
            if (c < 0x20) continue;
            if (space) {
                sb.append(' ');
                space = false;
            }
            sb.append(c);
        }
        return sb.toString();
    }

    /* ------------------------------------------------------------------ */
    /* The window.                                                         */
    /* ------------------------------------------------------------------ */

    private final class Panel extends Presentation {

        private InventoryView view;
        private int lastSerial;
        private boolean polling;

        private final Runnable poll = new Runnable() {
            @Override public void run() {
                if (!polling) return;
                try {
                    byte[] blob = nativePoll(lastSerial);
                    if (blob != null) {
                        Snapshot s = parse(blob);
                        if (s != null) {
                            lastSerial = s.serial;
                            view.setSnapshot(s);
                        }
                    }
                } catch (UnsatisfiedLinkError e) {
                    /* libmain.so is loaded by SDL a moment after the activity
                     * is created; until then there is simply nothing to show. */
                }
                handler.postDelayed(this, POLL_MS);
            }
        };

        Panel(Context outer, Display display) {
            super(outer, display);
        }

        @Override protected void onCreate(Bundle savedInstanceState) {
            super.onCreate(savedInstanceState);

            Window w = getWindow();
            if (w != null) {
                int flags = WindowManager.LayoutParams.FLAG_NOT_TOUCH_MODAL
                          | WindowManager.LayoutParams.FLAG_FULLSCREEN
                          | WindowManager.LayoutParams.FLAG_LAYOUT_IN_SCREEN
                          | WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON;
                if (!focusable) {
                    flags |= WindowManager.LayoutParams.FLAG_NOT_FOCUSABLE;
                }
                w.setFlags(flags, flags);
                w.setBackgroundDrawable(new android.graphics.drawable.ColorDrawable(Color.BLACK));

                WindowManager.LayoutParams lp = w.getAttributes();
                lp.width = WindowManager.LayoutParams.MATCH_PARENT;
                lp.height = WindowManager.LayoutParams.MATCH_PARENT;
                if (Build.VERSION.SDK_INT >= 28) {
                    lp.layoutInDisplayCutoutMode = (Build.VERSION.SDK_INT >= 30)
                            ? WindowManager.LayoutParams.LAYOUT_IN_DISPLAY_CUTOUT_MODE_ALWAYS
                            : WindowManager.LayoutParams.LAYOUT_IN_DISPLAY_CUTOUT_MODE_SHORT_EDGES;
                }
                w.setAttributes(lp);
            }

            view = new InventoryView(getContext());
            setContentView(view);
            setCancelable(false);
        }

        @Override protected void onStart() {
            super.onStart();
            applyImmersive();
            polling = true;
            handler.post(poll);
        }

        @Override protected void onStop() {
            polling = false;
            handler.removeCallbacks(poll);
            super.onStop();
        }

        @Override public void onWindowFocusChanged(boolean hasFocus) {
            super.onWindowFocusChanged(hasFocus);
            Log.i(TAG, "second screen window focus=" + hasFocus);
            if (hasFocus) {
                applyImmersive();
            }
        }

        /* Only reachable with focusable=1. The pad must keep driving the game,
         * so anything that lands here is handed straight to the game's window. */
        @Override public boolean dispatchKeyEvent(KeyEvent event) {
            return activity.dispatchKeyEvent(event);
        }

        @Override public boolean dispatchGenericMotionEvent(MotionEvent event) {
            return activity.dispatchGenericMotionEvent(event);
        }

        @SuppressWarnings("deprecation")
        void applyImmersive() {
            Window w = getWindow();
            if (w == null) return;
            View decor = w.getDecorView();

            if (Build.VERSION.SDK_INT >= 30) {
                w.setDecorFitsSystemWindows(false);
                WindowInsetsController c = decor.getWindowInsetsController();
                if (c != null) {
                    c.setSystemBarsBehavior(WindowInsetsController.BEHAVIOR_SHOW_TRANSIENT_BARS_BY_SWIPE);
                    c.hide(WindowInsets.Type.systemBars());
                }
            }

            /* Also the pre-11 flags: harmless on newer releases, and some
             * vendor builds still key their own bars off them. */
            decor.setSystemUiVisibility(
                    View.SYSTEM_UI_FLAG_LAYOUT_STABLE
                  | View.SYSTEM_UI_FLAG_LAYOUT_HIDE_NAVIGATION
                  | View.SYSTEM_UI_FLAG_LAYOUT_FULLSCREEN
                  | View.SYSTEM_UI_FLAG_HIDE_NAVIGATION
                  | View.SYSTEM_UI_FLAG_FULLSCREEN
                  | View.SYSTEM_UI_FLAG_IMMERSIVE_STICKY);
        }
    }

    /* ------------------------------------------------------------------ */
    /* The picture.                                                        */
    /* ------------------------------------------------------------------ */

    private final class InventoryView extends View {

        /* Everything is laid out in units of 1/1080 of the view's width, so
         * the 1080x1240 panel this was designed for is 1:1 and any other
         * panel scales instead of reflowing. */
        private float u = 1f;

        private final Paint fill = new Paint(Paint.ANTI_ALIAS_FLAG);
        private final Paint line = new Paint(Paint.ANTI_ALIAS_FLAG);
        private final Paint text = new Paint(Paint.ANTI_ALIAS_FLAG | Paint.SUBPIXEL_TEXT_FLAG);
        private final RectF r = new RectF();
        private final Path path = new Path();

        private Snapshot snap;
        private final boolean spanish;

        private float scrollY;
        private float maxScroll;
        private float lastTouchY;
        private boolean loggedLayout;
        private String insetsText = "";

        private static final int C_BG = 0xFF0B0B0D;
        private static final int C_PANEL = 0xFF17181C;
        private static final int C_PANEL_EQ = 0xFF2A2416;
        private static final int C_EDGE = 0xFF2C2E35;
        private static final int C_TEXT = 0xFFE9E6DF;
        private static final int C_DIM = 0xFF8D8A84;
        private static final int C_ACCENT = 0xFFD9A441;

        InventoryView(Context ctx) {
            super(ctx);
            spanish = "es".equals(Locale.getDefault().getLanguage());
            line.setStyle(Paint.Style.STROKE);
            text.setTypeface(Typeface.create("sans-serif-condensed", Typeface.NORMAL));
            setBackgroundColor(C_BG);
        }

        void setSnapshot(Snapshot s) {
            snap = s;
            invalidate();
        }

        @Override protected void onSizeChanged(int w, int h, int oldw, int oldh) {
            super.onSizeChanged(w, h, oldw, oldh);
            u = w / 1080f;
            Log.i(TAG, "second screen view " + w + "x" + h);
            loggedLayout = false;
        }

        @Override public WindowInsets onApplyWindowInsets(WindowInsets insets) {
            String t;
            if (Build.VERSION.SDK_INT >= 30) {
                android.graphics.Insets bars = insets.getInsets(WindowInsets.Type.systemBars());
                boolean nav = insets.isVisible(WindowInsets.Type.navigationBars());
                boolean status = insets.isVisible(WindowInsets.Type.statusBars());
                t = "bars L" + bars.left + " T" + bars.top + " R" + bars.right + " B" + bars.bottom
                        + " navVisible=" + nav + " statusVisible=" + status;
            } else {
                t = "insets L" + insets.getSystemWindowInsetLeft() + " T" + insets.getSystemWindowInsetTop()
                        + " R" + insets.getSystemWindowInsetRight() + " B" + insets.getSystemWindowInsetBottom();
            }
            if (!t.equals(insetsText)) {
                insetsText = t;
                Log.i(TAG, "second screen " + t);
                invalidate();
            }
            return super.onApplyWindowInsets(insets);
        }

        @Override public boolean onTouchEvent(MotionEvent e) {
            /* Phase 1: the only gesture is dragging the list when it is longer
             * than the screen. Nothing reaches the game from here. */
            switch (e.getActionMasked()) {
                case MotionEvent.ACTION_DOWN:
                    lastTouchY = e.getY();
                    return true;
                case MotionEvent.ACTION_MOVE: {
                    float dy = e.getY() - lastTouchY;
                    lastTouchY = e.getY();
                    float ns = Math.max(0f, Math.min(maxScroll, scrollY - dy));
                    if (ns != scrollY) {
                        scrollY = ns;
                        invalidate();
                    }
                    return true;
                }
                default:
                    return true;
            }
        }

        private String tr(String es, String en) {
            return spanish ? es : en;
        }

        private float fit(String s, float size, float maxWidth, float minSize) {
            text.setTextSize(size);
            float w = text.measureText(s);
            if (w > maxWidth && w > 0f) {
                size = Math.max(minSize, size * maxWidth / w);
                text.setTextSize(size);
            }
            return size;
        }

        private String ellipsize(String s, float maxWidth) {
            if (text.measureText(s) <= maxWidth) return s;
            int n = text.breakText(s, true, maxWidth - text.measureText("…"), null);
            if (n <= 0) return "…";
            return s.substring(0, n).trim() + "…";
        }

        @Override protected void onDraw(Canvas c) {
            final int w = getWidth();
            final int h = getHeight();

            if (!loggedLayout && w > 0) {
                loggedLayout = true;
                Log.i(TAG, "second screen first draw " + w + "x" + h + " " + insetsText);
            }

            if (snap == null || !snap.session) {
                drawIdle(c, w, h);
            } else {
                drawInventory(c, w, h);
            }

            if (debugOverlay) {
                text.setColor(0xFF40FF80);
                text.setTextSize(30 * u);
                text.setTextAlign(Paint.Align.LEFT);
                c.drawText(w + "x" + h + "  " + insetsText, 12 * u, h - 14 * u, text);
            }
        }

        private void drawIdle(Canvas c, int w, int h) {
            text.setTextAlign(Paint.Align.CENTER);
            text.setColor(C_DIM);
            text.setTextSize(84 * u * textScale);
            c.drawText("SILENT HILL", w / 2f, h / 2f - 20 * u, text);
            text.setTextSize(40 * u * textScale);
            c.drawText(tr("El inventario aparece al cargar una partida",
                          "The inventory appears once a game is loaded"),
                       w / 2f, h / 2f + 60 * u, text);
        }

        private void drawInventory(Canvas c, int w, int h) {
            final float pad = 24 * u;
            float y = pad;

            y = drawStatus(c, pad, y, w - 2 * pad);
            y += 16 * u;
            y = drawEquipped(c, pad, y, w - 2 * pad);
            y += 20 * u;

            /* Item grid: two columns. Rows are 112 units tall -- about 9 mm on
             * a 3.92" panel, which is what phase 2 needs for a touch target. */
            final float gap = 12 * u;
            final float cellW = (w - 2 * pad - gap) / 2f;
            final float cellH = 112 * u;
            final float top = y;
            final int n = snap.items.size();
            final int rows = (n + 1) / 2;
            final float contentH = rows * (cellH + gap);
            final float viewH = h - top - pad * 0.5f;

            maxScroll = Math.max(0f, contentH - viewH);
            if (scrollY > maxScroll) scrollY = maxScroll;

            c.save();
            c.clipRect(0, top, w, h);
            for (int i = 0; i < n; i++) {
                float cx = pad + (i % 2) * (cellW + gap);
                float cy = top + (i / 2) * (cellH + gap) - scrollY;
                if (cy + cellH < top || cy > h) continue;
                drawCell(c, snap.items.get(i), cx, cy, cellW, cellH);
            }
            c.restore();

            if (n == 0) {
                text.setTextAlign(Paint.Align.CENTER);
                text.setColor(C_DIM);
                text.setTextSize(40 * u * textScale);
                c.drawText(tr("Sin objetos", "No items"), w / 2f, top + 120 * u, text);
            }

            if (maxScroll > 0f) {
                float trackH = viewH;
                float thumbH = Math.max(60 * u, trackH * viewH / contentH);
                float ty = top + (trackH - thumbH) * (scrollY / maxScroll);
                fill.setColor(0x80D9A441);
                r.set(w - 10 * u, ty, w - 4 * u, ty + thumbH);
                c.drawRoundRect(r, 3 * u, 3 * u, fill);
            }
        }

        /* Health. The game never shows a number, only a colour that goes
         * green -> orange -> red at the same two thresholds used here (50 and
         * 10, Gfx_Inventory_HealthStatusDraw), so the bar follows those. */
        private float drawStatus(Canvas c, float x, float y, float w) {
            final float hgt = 150 * u;
            float pct = snap.healthQ12 / 4096f;
            if (pct < 0f) pct = 0f;
            if (pct > 100f) pct = 100f;

            int col;
            String word;
            if (pct < 10f) {
                col = 0xFFD43C3C;
                word = tr("PELIGRO", "DANGER");
            } else if (pct < 50f) {
                col = 0xFFE08A2E;
                word = tr("PRECAUCIÓN", "CAUTION");
            } else {
                col = 0xFF58B85C;
                word = tr("BIEN", "FINE");
            }

            fill.setColor(C_PANEL);
            r.set(x, y, x + w, y + hgt);
            c.drawRoundRect(r, 16 * u, 16 * u, fill);

            text.setTextAlign(Paint.Align.LEFT);
            text.setColor(C_DIM);
            text.setTextSize(34 * u * textScale);
            c.drawText(tr("ESTADO", "STATUS"), x + 28 * u, y + 52 * u, text);

            text.setColor(col);
            text.setFakeBoldText(true);
            text.setTextSize(52 * u * textScale);
            c.drawText(word, x + 190 * u, y + 58 * u, text);
            text.setFakeBoldText(false);

            text.setTextAlign(Paint.Align.RIGHT);
            text.setColor(C_TEXT);
            text.setTextSize(52 * u * textScale);
            c.drawText(Math.round(pct) + "%", x + w - 28 * u, y + 58 * u, text);

            float bx = x + 28 * u;
            float bw = w - 56 * u;
            float by = y + 86 * u;
            float bh = 36 * u;
            fill.setColor(0xFF2A2B30);
            r.set(bx, by, bx + bw, by + bh);
            c.drawRoundRect(r, 8 * u, 8 * u, fill);
            if (pct > 0f) {
                fill.setColor(col);
                r.set(bx, by, bx + Math.max(bh, bw * pct / 100f), by + bh);
                c.drawRoundRect(r, 8 * u, 8 * u, fill);
            }
            return y + hgt;
        }

        private float drawEquipped(Canvas c, float x, float y, float w) {
            final float hgt = 150 * u;
            fill.setColor(C_PANEL_EQ);
            r.set(x, y, x + w, y + hgt);
            c.drawRoundRect(r, 16 * u, 16 * u, fill);
            line.setColor(C_ACCENT);
            line.setStrokeWidth(3 * u);
            c.drawRoundRect(r, 16 * u, 16 * u, line);

            text.setTextAlign(Paint.Align.LEFT);
            text.setColor(C_ACCENT);
            text.setTextSize(30 * u * textScale);
            c.drawText(tr("EQUIPADO", "EQUIPPED"), x + 150 * u, y + 46 * u, text);

            Item eq = null;
            if (snap.equippedId != 0) {
                for (Item it : snap.items) {
                    if (it.slot == snap.equippedSlot) {
                        eq = it;
                        break;
                    }
                }
            }

            if (eq == null) {
                drawGlyph(c, 0, x + 76 * u, y + hgt / 2f, 44 * u, C_DIM);
                text.setColor(C_DIM);
                text.setTextSize(52 * u * textScale);
                c.drawText(tr("Sin arma", "No weapon"), x + 150 * u, y + 108 * u, text);
                return y + hgt;
            }

            drawGlyph(c, eq.id >> 5, x + 76 * u, y + hgt / 2f, 44 * u, C_ACCENT);

            boolean gun = (eq.id >> 5) == 5;
            float nameMax = w - 150 * u - (gun ? 330 * u : 28 * u);
            text.setColor(C_TEXT);
            fit(eq.name, 56 * u * textScale, nameMax, 34 * u);
            c.drawText(ellipsize(eq.name, nameMax), x + 150 * u, y + 110 * u, text);

            if (gun) {
                text.setTextAlign(Paint.Align.RIGHT);
                text.setColor(C_DIM);
                text.setTextSize(44 * u * textScale);
                String reserve = " / " + snap.ammoReserve;
                float rw = text.measureText(reserve);
                c.drawText(reserve, x + w - 28 * u, y + 104 * u, text);
                text.setColor(snap.ammoLoaded == 0 ? 0xFFD43C3C : C_TEXT);
                text.setFakeBoldText(true);
                text.setTextSize(84 * u * textScale);
                c.drawText(String.valueOf(snap.ammoLoaded), x + w - 28 * u - rw, y + 108 * u, text);
                text.setFakeBoldText(false);
            }
            return y + hgt;
        }

        private void drawCell(Canvas c, Item it, float x, float y, float w, float h) {
            int group = it.id >> 5;
            boolean equipped = snap.equippedId != 0 && it.slot == snap.equippedSlot;
            int col = groupColor(group);

            fill.setColor(equipped ? C_PANEL_EQ : C_PANEL);
            r.set(x, y, x + w, y + h);
            c.drawRoundRect(r, 14 * u, 14 * u, fill);
            line.setColor(equipped ? C_ACCENT : C_EDGE);
            line.setStrokeWidth((equipped ? 3 : 2) * u);
            c.drawRoundRect(r, 14 * u, 14 * u, line);

            drawGlyph(c, group, x + 56 * u, y + h / 2f, 30 * u, col);

            /* Right-hand tag: a count where a count means something, on/off
             * for the two switchable items. Keys and puzzle items are always
             * one of a kind and carry nothing. */
            String tag = null;
            int tagCol = C_TEXT;
            if (it.id == 224) {
                tag = snap.flashlightOn ? "ON" : "OFF";
                tagCol = snap.flashlightOn ? C_ACCENT : C_DIM;
            } else if (it.id == 225) {
                tag = snap.radioOn ? "ON" : "OFF";
                tagCol = snap.radioOn ? C_ACCENT : C_DIM;
            } else if (group == 1 || group == 5 || group == 6) {
                tag = String.valueOf(it.count);
                if (group == 5 && it.count == 0) tagCol = 0xFFD43C3C;
            }

            float tagW = 0f;
            if (tag != null) {
                text.setTextAlign(Paint.Align.RIGHT);
                text.setColor(tagCol);
                text.setFakeBoldText(true);
                text.setTextSize(48 * u * textScale);
                tagW = text.measureText(tag) + 18 * u;
                c.drawText(tag, x + w - 20 * u, y + h / 2f + 17 * u, text);
                text.setFakeBoldText(false);
            }

            float nameX = x + 104 * u;
            float nameMax = w - 104 * u - 20 * u - tagW;
            text.setTextAlign(Paint.Align.LEFT);
            text.setColor(C_TEXT);
            float size = fit(it.name, 42 * u * textScale, nameMax, 32 * u);
            c.drawText(ellipsize(it.name, nameMax), nameX, y + h / 2f + size * 0.35f, text);
        }

        private int groupColor(int group) {
            switch (group) {
                case 1: return 0xFF58B85C;
                case 2: return 0xFFD9B95A;
                case 3: return 0xFF9B8FD9;
                case 4: return 0xFFB9BDC6;
                case 5: return 0xFFE08A2E;
                case 6: return 0xFFC9A26B;
                case 7: return 0xFF6FB6D9;
                default: return C_DIM;
            }
        }

        /* One symbol per item group (e_InvItemGroup), drawn rather than
         * shipped: the game has no 2D item art, and nothing off the disc may
         * go in the APK. s is the half-size. */
        private void drawGlyph(Canvas c, int group, float cx, float cy, float s, int color) {
            fill.setColor(color);
            line.setColor(color);
            line.setStrokeWidth(Math.max(2f, s * 0.2f));
            line.setStrokeCap(Paint.Cap.ROUND);

            switch (group) {
                case 1: /* health: cross */
                    r.set(cx - s * 0.3f, cy - s, cx + s * 0.3f, cy + s);
                    c.drawRect(r, fill);
                    r.set(cx - s, cy - s * 0.3f, cx + s, cy + s * 0.3f);
                    c.drawRect(r, fill);
                    break;

                case 2: /* key */
                    c.drawCircle(cx - s * 0.5f, cy, s * 0.42f, line);
                    c.drawLine(cx - s * 0.08f, cy, cx + s, cy, line);
                    c.drawLine(cx + s * 0.55f, cy, cx + s * 0.55f, cy + s * 0.45f, line);
                    c.drawLine(cx + s * 0.95f, cy, cx + s * 0.95f, cy + s * 0.45f, line);
                    break;

                case 3: /* puzzle item: diamond */
                    path.reset();
                    path.moveTo(cx, cy - s);
                    path.lineTo(cx + s * 0.8f, cy);
                    path.lineTo(cx, cy + s);
                    path.lineTo(cx - s * 0.8f, cy);
                    path.close();
                    c.drawPath(path, line);
                    break;

                case 4: /* melee: blade and guard */
                    c.drawLine(cx - s * 0.8f, cy + s * 0.8f, cx + s * 0.8f, cy - s * 0.8f, line);
                    c.drawLine(cx - s * 0.75f, cy + s * 0.1f, cx - s * 0.1f, cy + s * 0.75f, line);
                    break;

                case 5: /* firearm: crosshair */
                    c.drawCircle(cx, cy, s * 0.62f, line);
                    c.drawLine(cx - s, cy, cx - s * 0.3f, cy, line);
                    c.drawLine(cx + s * 0.3f, cy, cx + s, cy, line);
                    c.drawLine(cx, cy - s, cx, cy - s * 0.3f, line);
                    c.drawLine(cx, cy + s * 0.3f, cx, cy + s, line);
                    break;

                case 6: /* ammunition: three rounds */
                    for (int i = -1; i <= 1; i++) {
                        float bx = cx + i * s * 0.62f;
                        r.set(bx - s * 0.2f, cy - s * 0.35f, bx + s * 0.2f, cy + s * 0.85f);
                        c.drawRect(r, fill);
                        c.drawCircle(bx, cy - s * 0.4f, s * 0.2f, fill);
                    }
                    break;

                case 7: /* portable: lamp with rays */
                    c.drawCircle(cx, cy + s * 0.15f, s * 0.42f, fill);
                    c.drawLine(cx, cy - s, cx, cy - s * 0.6f, line);
                    c.drawLine(cx - s * 0.85f, cy - s * 0.55f, cx - s * 0.55f, cy - s * 0.3f, line);
                    c.drawLine(cx + s * 0.85f, cy - s * 0.55f, cx + s * 0.55f, cy - s * 0.3f, line);
                    break;

                default: /* nothing equipped: empty ring */
                    c.drawCircle(cx, cy, s * 0.7f, line);
                    break;
            }
        }
    }
}
