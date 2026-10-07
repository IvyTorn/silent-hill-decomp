package com.silenthill.port;

import android.app.Activity;
import android.app.Presentation;
import android.content.Context;
import android.graphics.Bitmap;
import android.graphics.Canvas;
import android.graphics.Color;
import android.graphics.ColorMatrix;
import android.graphics.ColorMatrixColorFilter;
import android.graphics.LinearGradient;
import android.graphics.Paint;
import android.graphics.Path;
import android.graphics.Rect;
import android.graphics.RectF;
import android.graphics.Shader;
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
 * (pc_second_screen.c). Tapping an item asks the game to open its own
 * inventory on it; the command the player then picks here is pressed there, by
 * the game's own code.
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
    private static final int POLL_MS = 50;

    private final Activity activity;
    private final DisplayManager displayManager;
    private final Handler handler = new Handler(Looper.getMainLooper());

    private boolean enabled = true;
    private int wantedDisplayId = -1;
    private boolean focusable = false;
    private boolean debugOverlay = false;
    private float textScale = 1.0f;
    private boolean useGameFont = true;
    private boolean crtFollow = true;

    private Panel panel;
    private boolean started;
    private boolean listening;

    private static native byte[] nativePoll(int lastSerial);
    private static native void nativeInput(int kind, int a, int b);
    private static native byte[] nativeFont();

    /* Request kinds and driver states: the enums in pc_second_screen.c. */
    private static final int RQ_SELECT = 1, RQ_CHOOSE = 2, RQ_CANCEL = 3, RQ_DISMISS = 4;
    private static final int UI_IDLE = 0, UI_OPENING = 1, UI_SEEK = 2, UI_MENU = 3,
                             UI_VIEWING = 7, UI_SETTLE = 8, UI_CLOSING = 9;

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
        useGameFont = !"0".equals(p.getProperty("game_font", "1").trim());
        crtFollow = !"0".equals(p.getProperty("crt", "1").trim());

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

        Log.i(TAG, "build: fase3 (game font + crt)  config: game_font=" + useGameFont + " crt=" + crtFollow + " enabled=" + enabled + " display=" + disp + " focusable=" + focusable
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
            + "# Text size multiplier, 0.5 to 2.0. The game's font only scales by whole\n"
            + "# pixels, so small changes may do nothing.\n"
            + "text_scale=1.0\n"
            + "\n"
            + "# 1 = draw text with the game's own font, read from your disc at run time.\n"
            + "# 0 = use the system font.\n"
            + "game_font=1\n"
            + "\n"
            + "# 1 = put the game's post_process filter (CRT, Scanlines or Vignette) on the\n"
            + "#     second screen as well. 0 = never.\n"
            + "crt=1\n"
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
        int uiState;
        int uiSlot;
        int uiCmd;
        int eventSeq;
        int eventCode;
        int fontSerial;
        int postMode;
        int postMix;
        final List<Item> items = new ArrayList<Item>();
    }

    private static final Charset LATIN1 = Charset.forName("ISO-8859-1");

    static Snapshot parse(byte[] b) {
        final int head = 4 + 28;
        if (b == null || b.length < head) return null;
        if (b[4] != 'S' || b[5] != 'H' || b[6] != '2' || b[7] != 'S' || b[8] != 3) return null;

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
        s.uiState = b[22] & 0xFF;
        s.uiSlot = b[23] & 0xFF;
        s.uiCmd = b[24] & 0xFF;
        s.eventSeq = b[25] & 0xFF;
        s.eventCode = b[26] & 0xFF;
        s.fontSerial = b[27] & 0xFF;
        s.postMode = b[28] & 0xFF;
        s.postMix = b[29] & 0xFF;

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
    /* The game's own font.                                                */
    /* ------------------------------------------------------------------ */

    /**
     * FONT16 as the game uploaded it from the player's disc, rebuilt from the
     * blob pc_second_screen.c reads out of the emulated VRAM (layout in
     * Sf_Capture). Nothing of it is shipped in the APK.
     */
    static final class GameFont {
        static final int CELL_MAX = 126;

        int cols, rows, cellW, cellH, glyphCount, space;
        final int[] widths = new int[CELL_MAX];
        final byte[] map = new byte[256 * 7];
        Bitmap atlas;
        float inkedShare;

        static GameFont parse(byte[] b) {
            final int head = 12 + CELL_MAX + 256 * 7 + 32;
            if (b == null || b.length < head) return null;
            if (b[0] != 'S' || b[1] != 'H' || b[2] != 'F' || b[3] != '1') return null;

            GameFont f = new GameFont();
            f.cols = b[4] & 0xFF;
            f.rows = b[5] & 0xFF;
            f.cellW = b[6] & 0xFF;
            f.cellH = b[7] & 0xFF;
            f.glyphCount = b[8] & 0xFF;
            f.space = b[9] & 0xFF;
            if (f.cols <= 0 || f.rows <= 0 || f.cellW <= 0 || f.cellH <= 0 || f.glyphCount > CELL_MAX) return null;

            final int w = f.cols * f.cellW;
            final int h = f.rows * f.cellH;
            if (b.length < head + w * h) return null;

            for (int i = 0; i < CELL_MAX; i++) f.widths[i] = b[12 + i] & 0xFF;
            System.arraycopy(b, 12 + CELL_MAX, f.map, 0, 256 * 7);

            /* PSX 15-bit colour, and its one rule for texture transparency: a
             * texel whose whole colour word is zero is not drawn. */
            int[] clut = new int[16];
            int cp = 12 + CELL_MAX + 256 * 7;
            for (int i = 0; i < 16; i++) {
                int v = (b[cp + i * 2] & 0xFF) | ((b[cp + i * 2 + 1] & 0xFF) << 8);
                if (v == 0) {
                    clut[i] = 0;
                } else {
                    int r = v & 31, g = (v >> 5) & 31, bl = (v >> 10) & 31;
                    clut[i] = 0xFF000000 | (((r << 3) | (r >> 2)) << 16) | (((g << 3) | (g >> 2)) << 8) | ((bl << 3) | (bl >> 2));
                }
            }

            int[] px = new int[w * h];
            int inked = 0;
            for (int i = 0; i < w * h; i++) {
                int c = clut[b[head + i] & 0xF];
                px[i] = c;
                if (c != 0) inked++;
            }
            f.inkedShare = inked / (float) (w * h);
            f.atlas = Bitmap.createBitmap(px, w, h, Bitmap.Config.ARGB_8888);
            return f;
        }
    }

    /* ------------------------------------------------------------------ */
    /* The picture.                                                        */
    /* ------------------------------------------------------------------ */

    private final class InventoryView extends View {

        /* Geometry is in units of 1/1240 of the view's width when the panel is
         * wider than tall (the Thor's second screen reports 1240x1080, so that
         * is 1:1) and 1/1080 when it is taller than wide. Text is different:
         * it is the game's bitmap font, so it only ever scales by whole
         * pixels -- P real pixels per font texel -- or it would smear. */
        private float u = 1f;
        private boolean wide;
        private int P = 3;

        private final Paint fill = new Paint();
        private final Paint line = new Paint();
        private final Paint text = new Paint(Paint.ANTI_ALIAS_FLAG | Paint.SUBPIXEL_TEXT_FLAG);
        private final Paint glyphPaint = new Paint();
        private final RectF r = new RectF();
        private final Rect src = new Rect();
        private final Path path = new Path();
        private final java.util.HashMap<Integer, ColorMatrixColorFilter> tints =
                new java.util.HashMap<Integer, ColorMatrixColorFilter>();

        private Snapshot snap;
        private final boolean spanish;

        private GameFont font;
        private int fontSerial;

        private int fxMode = -1;
        private int fxMix = -1;
        private boolean fxShader;   /* the real filter is on the view */
        private boolean fxOverlay;  /* approximate it by hand instead */
        private final float[] pt = new float[2];

        private float scrollY;
        private float maxScroll;
        private float lastTouchY;
        private float downX;
        private float downY;
        private boolean dragging;

        /* Where things were last drawn, for taps. */
        private final List<RectF> cellRects = new ArrayList<RectF>();
        private final List<Item> cellItems = new ArrayList<Item>();
        private final RectF equippedRect = new RectF();
        private final RectF listRect = new RectF();
        private final RectF[] buttonRects = { new RectF(), new RectF(), new RectF() };
        private final int[] buttonActions = new int[3];
        private int buttonCount;

        /* A tap is drawn as "opening" at once rather than a poll later. */
        private Item pendingItem;
        private long pendingUntil;

        private int lastEventSeq = -1;
        private String toast;
        private long toastUntil;

        private boolean loggedLayout;
        private String insetsText = "";

        private static final int ACT_CANCEL = -1, ACT_DISMISS = -2;

        /* Text tints, in the game's own convention: a multiplier on the font
         * texel where 0x80 per channel leaves it unchanged. The first three
         * are rows of STRING_COLORS in text_draw.c. */
        private static final int T_WHITE = 0x808080;
        private static final int T_GOLD = 0xA08040;
        private static final int T_GREY = 0x505050;
        private static final int T_DIM = 0x303030;
        private static final int T_RED = 0xC02020;

        /* Health colours: D_80027F04, the table the status portrait uses. */
        private static final int H_FINE = 0x00FF00;
        private static final int H_CAUTION = 0xFFA000;
        private static final int H_DANGER = 0xFF0000;

        private static final int C_EDGE = 0xFF3A3A3A;
        private static final int C_GOLD = 0xFFC8A050;

        InventoryView(Context ctx) {
            super(ctx);
            spanish = "es".equals(Locale.getDefault().getLanguage());
            line.setStyle(Paint.Style.STROKE);
            line.setStrokeCap(Paint.Cap.BUTT);
            text.setTypeface(Typeface.create("sans-serif-condensed", Typeface.NORMAL));
            glyphPaint.setFilterBitmap(false);
            setBackgroundColor(Color.BLACK);
        }

        /* -------------------------------------------------------------- */
        /* State coming in.                                                */
        /* -------------------------------------------------------------- */

        void setSnapshot(Snapshot s) {
            if (lastEventSeq >= 0 && s.eventSeq != lastEventSeq) {
                showToast(eventText(s.eventCode));
            }
            lastEventSeq = s.eventSeq;
            if (s.uiState != UI_IDLE) {
                pendingItem = null;
            }
            snap = s;

            if (useGameFont && s.fontSerial != 0 && s.fontSerial != fontSerial) {
                fontSerial = s.fontSerial;
                loadFont();
            }
            if (s.postMode != fxMode || s.postMix != fxMix) {
                fxMode = s.postMode;
                fxMix = s.postMix;
                applyFx();
            }
            invalidate();
        }

        private void loadFont() {
            GameFont f = null;
            try {
                f = GameFont.parse(nativeFont());
            } catch (UnsatisfiedLinkError e) {
                f = null;
            } catch (RuntimeException e) {
                Log.w(TAG, "font: " + e);
            }
            if (f == null) {
                Log.w(TAG, "font: no usable blob; keeping the system font");
                return;
            }
            /* A wrong read of VRAM shows up as an atlas that is nearly empty
             * or nearly solid. Unreadable text is worse than the wrong font. */
            if (f.inkedShare < 0.02f || f.inkedShare > 0.85f) {
                Log.w(TAG, "font: rejected, inked share " + f.inkedShare);
                return;
            }
            font = f;
            Log.i(TAG, "font: game font in use, " + f.glyphCount + " glyphs, "
                    + f.cols + "x" + f.rows + " cells, inked " + Math.round(f.inkedShare * 100) + "%");
        }

        /* The port's post-process filter (PsyX_render.cpp, the u_postMode
         * branches) only ever sees the game's own framebuffer. Where the
         * platform can run a shader over a view -- Android 13 on -- the same
         * arithmetic is put over this one; modes 1 to 3, the ones that are
         * pure screen-space. Anything else, or an older Android, gets the
         * scanlines drawn by hand. */
        private void applyFx() {
            boolean want = crtFollow && fxMode >= 1 && fxMode <= 3 && fxMix > 0 && getWidth() > 0;

            fxShader = false;
            fxOverlay = false;

            if (Build.VERSION.SDK_INT >= 33) {
                if (want) {
                    try {
                        android.graphics.RuntimeShader sh = new android.graphics.RuntimeShader(FX_AGSL);
                        sh.setFloatUniform("size", (float) getWidth(), (float) getHeight());
                        sh.setFloatUniform("mode", (float) fxMode);
                        sh.setFloatUniform("amount", fxMix / 100f);
                        setRenderEffect(android.graphics.RenderEffect.createRuntimeShaderEffect(sh, "content"));
                        fxShader = true;
                    } catch (Throwable t) {
                        Log.w(TAG, "fx: shader refused (" + t + "); drawing scanlines by hand");
                        setRenderEffect(null);
                        fxOverlay = true;
                    }
                } else {
                    setRenderEffect(null);
                }
            } else {
                fxOverlay = want;
            }
            Log.i(TAG, "fx: post_process=" + fxMode + " mix=" + fxMix + "% -> "
                    + (fxShader ? "shader" : (fxOverlay ? "hand-drawn" : "none")));
        }

        /* No early return and no integer maths: the runtime-shader dialect is
         * the strict one, and a filter that fails to compile is no filter. */
        private static final String FX_AGSL =
              "uniform shader content;\n"
            + "uniform float2 size;\n"
            + "uniform float mode;\n"
            + "uniform float amount;\n"
            + "half4 main(float2 fc) {\n"
            + "  float2 uv = fc / size;\n"
            + "  float3 orig = content.eval(fc).rgb;\n"
            + "  float3 col = orig;\n"
            + "  float inside = 1.0;\n"
            + "  if (mode < 1.5) {\n"
            + "    float2 c = uv * 2.0 - 1.0;\n"
            + "    float2 o = abs(c.yx) / float2(6.0, 5.0);\n"
            + "    c += c * o * o;\n"
            + "    uv = c * 0.5 + 0.5;\n"
            + "    inside = step(0.0, uv.x) * step(uv.x, 1.0) * step(0.0, uv.y) * step(uv.y, 1.0);\n"
            + "    col = content.eval(uv * size).rgb;\n"
            + "    col *= 0.75 + 0.25 * abs(sin(uv.y * 240.0 * 3.14159));\n"
            + "    float m = mod(floor(fc.x), 3.0);\n"
            + "    float3 mask = float3(0.72, 0.72, 1.0);\n"
            + "    if (m < 0.5) { mask = float3(1.0, 0.72, 0.72); } else if (m < 1.5) { mask = float3(0.72, 1.0, 0.72); }\n"
            + "    col *= mask * 1.25;\n"
            + "    float2 d = uv - 0.5;\n"
            + "    col *= clamp(1.0 - dot(d, d) * 1.1, 0.0, 1.0);\n"
            + "  } else if (mode < 2.5) {\n"
            + "    col *= 0.7 + 0.3 * abs(sin(uv.y * 240.0 * 3.14159));\n"
            + "  } else {\n"
            + "    float2 d = uv - 0.5;\n"
            + "    col *= clamp(1.0 - dot(d, d) * 1.3, 0.0, 1.0);\n"
            + "  }\n"
            + "  col = mix(orig, col, amount) * inside;\n"
            + "  return half4(clamp(col, 0.0, 1.0), 1.0);\n"
            + "}\n";

        /** Where a touch at (x, y) on the glass lands in the undistorted layout. */
        private void unwarp(float x, float y) {
            pt[0] = x;
            pt[1] = y;
            if (!fxShader || fxMode != 1 || getWidth() <= 0 || getHeight() <= 0) return;
            float cx = x / getWidth() * 2f - 1f;
            float cy = y / getHeight() * 2f - 1f;
            float ox = Math.abs(cy) / 6f;
            float oy = Math.abs(cx) / 5f;
            cx += cx * ox * ox;
            cy += cy * oy * oy;
            pt[0] = (cx * 0.5f + 0.5f) * getWidth();
            pt[1] = (cy * 0.5f + 0.5f) * getHeight();
        }

        private void showToast(String t) {
            toast = t;
            toastUntil = android.os.SystemClock.uptimeMillis() + 2200;
            pendingItem = null;
            postInvalidateDelayed(2250);
        }

        private String eventText(int code) {
            switch (code) {
                case 2: return tr("No se puede usar aquí", "Can't be used here");
                case 3: return tr("Demasiado oscuro para examinarlo", "Too dark to look at it");
                case 4: return tr("El juego no respondió", "The game did not respond");
                default: return tr("No disponible ahora", "Not available right now");
            }
        }

        private Item itemAtSlot(int slot) {
            if (snap == null) return null;
            for (Item it : snap.items) {
                if (it.slot == slot) return it;
            }
            return null;
        }

        /** The item the sheet is about, or null when no sheet should show. */
        private Item sheetItem() {
            if (snap == null || !snap.session) return null;
            if (snap.uiState != UI_IDLE) {
                Item it = itemAtSlot(snap.uiSlot);
                return (it != null) ? it : pendingItem;
            }
            if (pendingItem != null && android.os.SystemClock.uptimeMillis() < pendingUntil) {
                return pendingItem;
            }
            pendingItem = null;
            return null;
        }

        /* -------------------------------------------------------------- */
        /* Touch.                                                          */
        /* -------------------------------------------------------------- */

        private void requestItem(Item it) {
            if (it == null) return;
            pendingItem = it;
            pendingUntil = android.os.SystemClock.uptimeMillis() + 2500;
            postInvalidateDelayed(2550);
            Log.i(TAG, "tap: slot=" + it.slot + " id=" + it.id + " '" + it.name + "'");
            try {
                nativeInput(RQ_SELECT, it.slot, it.id);
            } catch (UnsatisfiedLinkError e) {
                pendingItem = null;
            }
            invalidate();
        }

        private void pressButton(int action) {
            try {
                if (action == ACT_CANCEL) {
                    pendingItem = null;
                    nativeInput(RQ_CANCEL, 0, 0);
                } else if (action == ACT_DISMISS) {
                    nativeInput(RQ_DISMISS, 0, 0);
                } else {
                    Log.i(TAG, "tap: command row " + action);
                    nativeInput(RQ_CHOOSE, action, 0);
                }
            } catch (UnsatisfiedLinkError e) {
                Log.w(TAG, "native input unavailable");
            }
            invalidate();
        }

        private void handleTap(float x, float y) {
            if (snap == null || !snap.session) return;

            if (sheetItem() != null) {
                /* The sheet is modal: only its buttons take taps. */
                for (int i = 0; i < buttonCount; i++) {
                    if (buttonRects[i].contains(x, y)) {
                        pressButton(buttonActions[i]);
                        return;
                    }
                }
                return;
            }

            if (equippedRect.contains(x, y) && snap.equippedId != 0) {
                requestItem(itemAtSlot(snap.equippedSlot));
                return;
            }
            if (!listRect.contains(x, y)) return;
            for (int i = 0; i < cellRects.size(); i++) {
                if (cellRects.get(i).contains(x, y)) {
                    requestItem(cellItems.get(i));
                    return;
                }
            }
        }

        @Override public boolean onTouchEvent(MotionEvent e) {
            /* The CRT filter bends the picture, so a finger is over whatever
             * the bend put there, not what was laid out at that spot. */
            unwarp(e.getX(), e.getY());
            final float x = pt[0];
            final float y = pt[1];

            switch (e.getActionMasked()) {
                case MotionEvent.ACTION_DOWN:
                    downX = x;
                    downY = y;
                    lastTouchY = y;
                    dragging = false;
                    return true;

                case MotionEvent.ACTION_MOVE: {
                    /* A finger that travels is scrolling the list, not
                     * choosing from it. */
                    if (!dragging && Math.abs(y - downY) > 22 * u) {
                        dragging = true;
                        lastTouchY = y;
                    }
                    if (dragging && sheetItem() == null) {
                        float dy = y - lastTouchY;
                        lastTouchY = y;
                        float ns = Math.max(0f, Math.min(maxScroll, scrollY - dy));
                        if (ns != scrollY) {
                            scrollY = ns;
                            invalidate();
                        }
                    }
                    return true;
                }

                case MotionEvent.ACTION_UP:
                    if (!dragging && Math.abs(x - downX) <= 22 * u) {
                        handleTap(x, y);
                    }
                    return true;

                default:
                    return true;
            }
        }

        /* -------------------------------------------------------------- */
        /* Layout plumbing.                                                */
        /* -------------------------------------------------------------- */

        @Override protected void onSizeChanged(int w, int h, int oldw, int oldh) {
            super.onSizeChanged(w, h, oldw, oldh);
            wide = w >= h;
            u = w / (wide ? 1240f : 1080f);
            P = Math.max(2, Math.round(Math.min(w, h) / 360f));
            Log.i(TAG, "second screen view " + w + "x" + h);
            loggedLayout = false;
            if (fxMode >= 0) {
                applyFx();
            }
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

        private String tr(String es, String en) {
            return spanish ? es : en;
        }

        /* -------------------------------------------------------------- */
        /* Text. px is real pixels per font texel.                         */
        /* -------------------------------------------------------------- */

        private int px(int want) {
            return Math.max(2, Math.round(want * textScale));
        }

        private float textWidth(String s, int px) {
            if (font == null) {
                text.setTextSize(px * 13f);
                return text.measureText(s);
            }
            int wTexels = 0;
            for (int i = 0; i < s.length(); i++) {
                int ch = s.charAt(i);
                if (ch == ' ') {
                    wTexels += font.space;
                    continue;
                }
                if (ch > 255) continue;
                int m = ch * 7;
                int n = font.map[m];
                for (int k = 0; k < n && k < 2; k++) {
                    wTexels += font.map[m + 3 + k * 3] & 0xFF;
                }
            }
            return wTexels * (float) px;
        }

        private ColorMatrixColorFilter tintFilter(int tint) {
            ColorMatrixColorFilter f = tints.get(tint);
            if (f == null) {
                ColorMatrix m = new ColorMatrix();
                m.setScale(((tint >> 16) & 0xFF) / 128f, ((tint >> 8) & 0xFF) / 128f, (tint & 0xFF) / 128f, 1f);
                f = new ColorMatrixColorFilter(m);
                tints.put(tint, f);
            }
            return f;
        }

        private int tintToColor(int tint) {
            int rr = Math.min(255, ((tint >> 16) & 0xFF) * 224 / 128);
            int gg = Math.min(255, ((tint >> 8) & 0xFF) * 224 / 128);
            int bb = Math.min(255, (tint & 0xFF) * 224 / 128);
            return 0xFF000000 | (rr << 16) | (gg << 8) | bb;
        }

        /** Draws s with its cell top at y. align applies to x. */
        private void drawString(Canvas c, String s, float x, float y, int px, int tint, Paint.Align align) {
            float w = textWidth(s, px);
            if (align == Paint.Align.CENTER) x -= w / 2f;
            else if (align == Paint.Align.RIGHT) x -= w;
            x = Math.round(x);
            y = Math.round(y);

            if (font == null) {
                text.setTextAlign(Paint.Align.LEFT);
                text.setTextSize(px * 13f);
                text.setColor(tintToColor(tint));
                c.drawText(s, x, y + px * 12.5f, text);
                return;
            }

            glyphPaint.setColorFilter(tintFilter(tint));
            for (int i = 0; i < s.length(); i++) {
                int ch = s.charAt(i);
                if (ch == ' ') {
                    x += font.space * px;
                    continue;
                }
                if (ch > 255) continue;
                int m = ch * 7;
                int n = font.map[m];
                for (int k = 0; k < n && k < 2; k++) {
                    int cell = font.map[m + 1 + k * 3] & 0xFF;
                    int dy = font.map[m + 2 + k * 3];
                    int adv = font.map[m + 3 + k * 3] & 0xFF;
                    if (cell < font.glyphCount) {
                        int sx = (cell % font.cols) * font.cellW;
                        int sy = (cell / font.cols) * font.cellH;
                        src.set(sx, sy, sx + font.cellW, sy + font.cellH);
                        r.set(x, y + dy * px, x + font.cellW * px, y + (dy + font.cellH) * px);
                        c.drawBitmap(font.atlas, src, r, glyphPaint);
                    }
                    x += adv * px;
                }
            }
        }

        /** The largest size from want down to min at which s fits in maxW. */
        private int fitPx(String s, int want, int min, float maxW) {
            for (int p = want; p > min; p--) {
                if (textWidth(s, p) <= maxW) return p;
            }
            return min;
        }

        private String ellipsize(String s, int px, float maxW) {
            if (textWidth(s, px) <= maxW) return s;
            String dots = "...";
            int n = s.length();
            while (n > 1 && textWidth(s.substring(0, n).trim() + dots, px) > maxW) n--;
            return s.substring(0, n).trim() + dots;
        }

        private int lineH(int px) {
            return px * 16;
        }

        /* -------------------------------------------------------------- */
        /* Shapes. No rounded corners and no anti-aliasing: the stock      */
        /* screen is flat gouraud quads and one-pixel lines.               */
        /* -------------------------------------------------------------- */

        private void rect(Canvas c, float l, float t, float rr, float b, int color) {
            fill.setShader(null);
            fill.setColor(color);
            r.set(Math.round(l), Math.round(t), Math.round(rr), Math.round(b));
            c.drawRect(r, fill);
        }

        /** A quad shaded from one colour at its left edge to another at its right. */
        private void gradient(Canvas c, float l, float t, float rr, float b, int from, int to) {
            fill.setShader(new LinearGradient(l, 0, rr, 0, from, to, Shader.TileMode.CLAMP));
            r.set(Math.round(l), Math.round(t), Math.round(rr), Math.round(b));
            c.drawRect(r, fill);
            fill.setShader(null);
        }

        private void frame(Canvas c, float l, float t, float rr, float b, int color) {
            rect(c, l, t, rr, t + P, color);
            rect(c, l, b - P, rr, b, color);
            rect(c, l, t, l + P, b, color);
            rect(c, rr - P, t, rr, b, color);
        }

        /** The stock selection outline: four corner brackets. */
        private void brackets(Canvas c, float l, float t, float rr, float b, int color) {
            float len = 26 * u;
            rect(c, l, t, l + len, t + P, color);
            rect(c, l, t, l + P, t + len, color);
            rect(c, rr - len, t, rr, t + P, color);
            rect(c, rr - P, t, rr, t + len, color);
            rect(c, l, b - P, l + len, b, color);
            rect(c, l, b - len, l + P, b, color);
            rect(c, rr - len, b - P, rr, b, color);
            rect(c, rr - P, b - len, rr, b, color);
        }

        /* -------------------------------------------------------------- */
        /* Drawing.                                                        */
        /* -------------------------------------------------------------- */

        @Override protected void onDraw(Canvas c) {
            final int w = getWidth();
            final int h = getHeight();

            if (!loggedLayout && w > 0) {
                loggedLayout = true;
                Log.i(TAG, "second screen first draw " + w + "x" + h + " " + insetsText);
            }

            buttonCount = 0;
            if (snap == null || !snap.session) {
                drawIdle(c, w, h);
            } else {
                drawInventory(c, w, h);
                Item sheet = sheetItem();
                if (sheet != null) {
                    drawSheet(c, w, h, sheet);
                }
                if (toast != null) {
                    if (android.os.SystemClock.uptimeMillis() < toastUntil) {
                        drawToast(c, w, h);
                    } else {
                        toast = null;
                    }
                }
            }

            if (fxOverlay) {
                drawScanlines(c, w, h);
            }

            if (debugOverlay) {
                text.setColor(0xFF40FF80);
                text.setTextSize(30 * u);
                text.setTextAlign(Paint.Align.LEFT);
                c.drawText(w + "x" + h + "  " + insetsText, 12 * u, h - 14 * u, text);
            }
        }

        /* The filter's scanline term, 240 lines down the picture, for when the
         * shader itself is not available. Curvature and the phosphor mask are
         * left out: neither can be had from plain drawing. */
        private void drawScanlines(Canvas c, int w, int h) {
            if (fxMode != 1 && fxMode != 2) return;
            float depth = (fxMode == 1 ? 0.25f : 0.30f) * fxMix / 100f;
            int color = (Math.round(depth * 255f) << 24);
            float period = h / 240f;
            fill.setShader(null);
            fill.setColor(color);
            for (int i = 0; i < 240; i++) {
                float top = i * period;
                r.set(0, top, w, top + period * 0.5f);
                c.drawRect(r, fill);
            }
        }

        private void drawIdle(Canvas c, int w, int h) {
            int big = px(P + 2);
            drawString(c, "SILENT HILL", w / 2f, h / 2f - lineH(big), big, T_GREY, Paint.Align.CENTER);
            String hint = tr("El inventario aparece al cargar una partida",
                             "The inventory appears once a game is loaded");
            int small = fitPx(hint, px(P - 1), 2, w - 80 * u);
            drawString(c, hint, w / 2f, h / 2f + 30 * u, small, T_DIM, Paint.Align.CENTER);
        }

        private void drawInventory(Canvas c, int w, int h) {
            final float pad = 24 * u;
            final float gap = 12 * u;
            float y = pad;

            if (wide) {
                float half = (w - 2 * pad - gap) / 2f;
                drawStatus(c, pad, y, half);
                equippedRect.set(pad + half + gap, y, pad + half + gap + half, y + 150 * u);
                y = drawEquipped(c, pad + half + gap, y, half);
                y += gap;
            } else {
                y = drawStatus(c, pad, y, w - 2 * pad);
                y += 16 * u;
                equippedRect.set(pad, y, w - pad, y + 150 * u);
                y = drawEquipped(c, pad, y, w - 2 * pad);
                y += 20 * u;
            }

            /* Item grid: two columns. Rows are 112 units tall -- just under
             * 7 mm on a 3.92" panel, and nearly the full half-width wide,
             * which is what a finger needs. */
            final float cellW = (w - 2 * pad - gap) / 2f;
            final float cellH = 112 * u;
            final float top = y;
            final int n = snap.items.size();
            final int rows = (n + 1) / 2;
            final float contentH = rows * (cellH + gap);
            final float viewH = h - top - pad * 0.5f;

            maxScroll = Math.max(0f, contentH - viewH);
            if (scrollY > maxScroll) scrollY = maxScroll;

            listRect.set(0, top, w, h);
            cellRects.clear();
            cellItems.clear();

            c.save();
            c.clipRect(0, top, w, h);
            for (int i = 0; i < n; i++) {
                float cx = pad + (i % 2) * (cellW + gap);
                float cy = top + (i / 2) * (cellH + gap) - scrollY;
                if (cy + cellH < top || cy > h) continue;
                Item it = snap.items.get(i);
                drawCell(c, it, cx, cy, cellW, cellH);
                cellRects.add(new RectF(cx, cy, cx + cellW, cy + cellH));
                cellItems.add(it);
            }
            c.restore();

            if (n == 0) {
                drawString(c, tr("Sin objetos", "No items"), w / 2f, top + 100 * u, px(P), T_GREY, Paint.Align.CENTER);
            }

            if (maxScroll > 0f) {
                float trackH = viewH;
                float thumbH = Math.max(60 * u, trackH * viewH / contentH);
                float ty = top + (trackH - thumbH) * (scrollY / maxScroll);
                rect(c, w - 9 * u, ty, w - 9 * u + P, ty + thumbH, C_GOLD);
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
                col = H_DANGER;
                word = tr("Peligro", "Danger");
            } else if (pct < 50f) {
                col = H_CAUTION;
                word = tr("Precaución", "Caution");
            } else {
                col = H_FINE;
                word = tr("Bien", "Fine");
            }
            /* As a text tint the full-bright table colour would clip the
             * font's shading flat; halve it into the multiplier's range. */
            int tint = ((col >> 1) & 0x7F7F7F) + 0x101010;

            brackets(c, x, y, x + w, y + hgt, C_EDGE);

            int small = px(P - 1);
            int big = px(P);
            drawString(c, tr("ESTADO", "STATUS"), x + 24 * u, y + 14 * u, small, T_GREY, Paint.Align.LEFT);

            String num = String.valueOf(Math.round(pct));
            float numW = textWidth(num, big);
            drawString(c, num, x + w - 24 * u, y + 52 * u, big, T_WHITE, Paint.Align.RIGHT);
            int wordPx = fitPx(word, big, 2, w - 48 * u - numW - 20 * u);
            drawString(c, word, x + 24 * u, y + 52 * u, wordPx, tint, Paint.Align.LEFT);

            float bx = x + 24 * u;
            float bw = w - 48 * u;
            float by = y + 112 * u;
            float bh = 22 * u;
            frame(c, bx, by, bx + bw, by + bh, C_EDGE);
            if (pct > 0f) {
                int full = 0xFF000000 | col;
                int dark = 0xFF000000 | ((col >> 2) & 0x3F3F3F);
                float fw = Math.max(P * 2, (bw - 2 * P) * pct / 100f);
                gradient(c, bx + P, by + P, bx + P + fw, by + bh - P, dark, full);
            }
            return y + hgt;
        }

        private float drawEquipped(Canvas c, float x, float y, float w) {
            final float hgt = 150 * u;

            gradient(c, x, y, x + w * 0.6f, y + hgt, 0x40A08040, 0x00A08040);
            brackets(c, x, y, x + w, y + hgt, C_GOLD);

            int small = px(P - 1);
            drawString(c, tr("EQUIPADO", "EQUIPPED"), x + 140 * u, y + 14 * u, small, T_GOLD, Paint.Align.LEFT);

            Item eq = null;
            if (snap.equippedId != 0) {
                eq = itemAtSlot(snap.equippedSlot);
            }

            if (eq == null) {
                drawGlyph(c, 0, x + 72 * u, y + hgt / 2f, 40 * u, 0xFF606060);
                drawString(c, tr("Sin arma", "No weapon"), x + 140 * u, y + 62 * u, px(P), T_GREY, Paint.Align.LEFT);
                return y + hgt;
            }

            drawGlyph(c, eq.id >> 5, x + 72 * u, y + hgt / 2f, 40 * u, C_GOLD);

            boolean gun = (eq.id >> 5) == 5;
            float ammoW = 0f;

            if (gun) {
                int rp = px(P);
                int lp = px(P + 2);
                String reserve = "/" + snap.ammoReserve;
                String loaded = String.valueOf(snap.ammoLoaded);
                float rw = textWidth(reserve, rp);
                float lw = textWidth(loaded, lp);
                float base = y + hgt - 18 * u;
                drawString(c, reserve, x + w - 24 * u, base - lineH(rp), rp, T_GREY, Paint.Align.RIGHT);
                drawString(c, loaded, x + w - 24 * u - rw - 6 * u, base - lineH(lp), lp,
                           snap.ammoLoaded == 0 ? T_RED : T_WHITE, Paint.Align.RIGHT);
                ammoW = rw + lw + 30 * u;
            }

            float nameMax = w - 140 * u - 24 * u - ammoW;
            int np = fitPx(eq.name, px(P), 2, nameMax);
            drawString(c, ellipsize(eq.name, np, nameMax), x + 140 * u, y + 62 * u + (lineH(px(P)) - lineH(np)) / 2f,
                       np, T_WHITE, Paint.Align.LEFT);
            return y + hgt;
        }

        private void drawCell(Canvas c, Item it, float x, float y, float w, float h) {
            int group = it.id >> 5;
            boolean equipped = snap.equippedId != 0 && it.slot == snap.equippedSlot;
            int col = groupColor(group);

            /* A faint wash of the group's colour dying away to the right, the
             * way the stock screen shades its Option / Exit / Map tabs. */
            gradient(c, x, y, x + w * 0.5f, y + h, (col & 0x00FFFFFF) | 0x30000000, col & 0x00FFFFFF);
            rect(c, x, y + h - P, x + w, y + h, 0xFF1E1E1E);
            if (equipped) {
                brackets(c, x, y, x + w, y + h, C_GOLD);
            }

            drawGlyph(c, group, x + 54 * u, y + h / 2f, 28 * u, col);

            /* Right-hand tag: a count where a count means something, on/off
             * for the two switchable items. Keys and puzzle items are always
             * one of a kind and carry nothing. */
            String tag = null;
            int tagTint = T_WHITE;
            if (it.id == 224) {
                tag = snap.flashlightOn ? "ON" : "OFF";
                tagTint = snap.flashlightOn ? T_GOLD : T_DIM;
            } else if (it.id == 225) {
                tag = snap.radioOn ? "ON" : "OFF";
                tagTint = snap.radioOn ? T_GOLD : T_DIM;
            } else if (group == 1 || group == 5 || group == 6) {
                tag = String.valueOf(it.count);
                if (group == 5 && it.count == 0) tagTint = T_RED;
            }

            int np = px(P);
            float top = y + (h - lineH(np)) / 2f;
            float tagW = 0f;
            if (tag != null) {
                tagW = textWidth(tag, np) + 16 * u;
                drawString(c, tag, x + w - 18 * u, top, np, tagTint, Paint.Align.RIGHT);
            }

            float nameX = x + 100 * u;
            float nameMax = w - 100 * u - 18 * u - tagW;
            int fp = fitPx(it.name, np, 2, nameMax);
            drawString(c, ellipsize(it.name, fp, nameMax), nameX, y + (h - lineH(fp)) / 2f, fp,
                       equipped ? T_GOLD : T_WHITE, Paint.Align.LEFT);
        }

        /* The command sheet. The rows are the stock screen's own, in its own
         * order (Gfx_Inventory_CmdOptionsDraw); the index a button sends back
         * is the row the game then presses. */
        private String[] commandLabels(int cmd) {
            String use = tr("Usar", "Use");
            String equip = tr("Equipar", "Equip");
            String unequip = tr("Quitar", "Unequip");
            String reload = tr("Recargar", "Reload");
            String look = tr("Examinar", "Look");
            switch (cmd) {
                case 0:
                case 1: return new String[] { use };
                case 2: return new String[] { equip };
                case 3: return new String[] { unequip };
                case 4: return new String[] { equip, reload };
                case 5: return new String[] { unequip, reload };
                case 6: return new String[] { tr("Encender", "On"), tr("Apagar", "Off") };
                case 7: return new String[] { reload };
                case 8: return new String[] { look };
                case 9: return new String[] { use, look };
                default: return new String[0];
            }
        }

        private void addButton(Canvas c, float x, float y, float w, float h, String label, int action, boolean primary) {
            if (buttonCount >= buttonRects.length) return;
            buttonRects[buttonCount].set(x, y, x + w, y + h);
            buttonActions[buttonCount] = action;
            buttonCount++;

            if (primary) {
                /* The stock command highlight: a bar of light fading out. */
                gradient(c, x, y, x + w / 2f, y + h, 0x00A08040, 0x70A08040);
                gradient(c, x + w / 2f, y, x + w, y + h, 0x70A08040, 0x00A08040);
                rect(c, x, y, x + w, y + P, C_GOLD);
                rect(c, x, y + h - P, x + w, y + h, C_GOLD);
            } else {
                frame(c, x, y, x + w, y + h, C_EDGE);
            }
            int p = fitPx(label, px(primary ? P + 1 : P), 2, w - 40 * u);
            drawString(c, label, x + w / 2f, y + (h - lineH(p)) / 2f, p, primary ? T_WHITE : T_GREY, Paint.Align.CENTER);
        }

        private void drawSheet(Canvas c, int w, int h, Item item) {
            int state = (snap.uiState == UI_IDLE) ? UI_OPENING : snap.uiState;

            rect(c, 0, 0, w, h, 0xD9000000);

            String[] labels = (state == UI_MENU) ? commandLabels(snap.uiCmd) : new String[0];
            String note = null;
            String closeLabel = null;
            int closeAction = ACT_CANCEL;

            if (state == UI_OPENING || state == UI_SEEK) {
                note = tr("Abriendo el inventario...", "Opening the inventory...");
                closeLabel = tr("Cancelar", "Cancel");
            } else if (state == UI_MENU) {
                if (labels.length == 0) {
                    note = tr("No se puede usar aquí", "Can't be used here");
                    closeLabel = tr("Volver", "Back");
                } else {
                    closeLabel = tr("Cancelar", "Cancel");
                }
            } else if (state == UI_VIEWING) {
                note = tr("Se muestra en la pantalla principal", "Shown on the main screen");
                closeLabel = tr("Cerrar", "Close");
                closeAction = ACT_DISMISS;
            } else if (state == UI_CLOSING || state == UI_SETTLE) {
                note = tr("Volviendo al juego...", "Returning to the game...");
            } else {
                note = tr("Un momento...", "One moment...");
            }

            final float pw = Math.min(w - 120 * u, 960 * u);
            final float bh = 140 * u;
            final float bgap = 22 * u;
            float ph = 150 * u
                     + (note != null ? 80 * u : 0f)
                     + labels.length * (bh + bgap)
                     + (closeLabel != null ? (110 * u + bgap) : 0f)
                     + 30 * u;
            float pxl = Math.round((w - pw) / 2f);
            float py = Math.round(Math.max(20 * u, (h - ph) / 2f));

            rect(c, pxl, py, pxl + pw, py + ph, 0xFF000000);
            frame(c, pxl, py, pxl + pw, py + ph, C_EDGE);
            brackets(c, pxl - 2 * P, py - 2 * P, pxl + pw + 2 * P, py + ph + 2 * P, C_GOLD);

            drawGlyph(c, item.id >> 5, pxl + 84 * u, py + 80 * u, 36 * u, groupColor(item.id >> 5));
            float nameMax = pw - 156 * u - 36 * u;
            int np = fitPx(item.name, px(P + 1), 2, nameMax);
            drawString(c, ellipsize(item.name, np, nameMax), pxl + 156 * u, py + 80 * u - lineH(np) / 2f, np, T_WHITE, Paint.Align.LEFT);

            float y = py + 150 * u;

            if (note != null) {
                int sp = fitPx(note, px(P), 2, pw - 60 * u);
                drawString(c, note, pxl + pw / 2f, y, sp, T_GREY, Paint.Align.CENTER);
                y += 80 * u;
            }

            for (int i = 0; i < labels.length; i++) {
                addButton(c, pxl + 36 * u, y, pw - 72 * u, bh, labels[i], i, true);
                y += bh + bgap;
            }

            if (closeLabel != null) {
                addButton(c, pxl + 36 * u, y, pw - 72 * u, 110 * u, closeLabel, closeAction, false);
            }
        }

        private void drawToast(Canvas c, int w, int h) {
            int p = fitPx(toast, px(P), 2, w - 160 * u);
            float tw = textWidth(toast, p) + 80 * u;
            float th = lineH(p) + 50 * u;
            float ty = h - th - 40 * u;
            rect(c, (w - tw) / 2f, ty, (w + tw) / 2f, ty + th, 0xFF000000);
            frame(c, (w - tw) / 2f, ty, (w + tw) / 2f, ty + th, 0xFFC02020);
            drawString(c, toast, w / 2f, ty + 25 * u, p, T_RED, Paint.Align.CENTER);
        }

        private int groupColor(int group) {
            switch (group) {
                case 1: return 0xFF30B040;
                case 2: return 0xFFB09040;
                case 3: return 0xFF8070B0;
                case 4: return 0xFF909090;
                case 5: return 0xFFB06828;
                case 6: return 0xFFA08050;
                case 7: return 0xFF4890B0;
                default: return 0xFF606060;
            }
        }

        /* One symbol per item group (e_InvItemGroup), drawn rather than
         * shipped: the game has no 2D item art. s is the half-size. */
        private void drawGlyph(Canvas c, int group, float cx, float cy, float s, int color) {
            fill.setShader(null);
            fill.setColor(color);
            line.setColor(color);
            line.setStrokeWidth(Math.max(P, Math.round(s * 0.2f)));

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
                        r.set(bx - s * 0.2f, cy - s * 0.55f, bx + s * 0.2f, cy + s * 0.85f);
                        c.drawRect(r, fill);
                    }
                    break;

                case 7: /* portable: lamp with rays */
                    r.set(cx - s * 0.4f, cy - s * 0.2f, cx + s * 0.4f, cy + s * 0.6f);
                    c.drawRect(r, fill);
                    c.drawLine(cx, cy - s, cx, cy - s * 0.55f, line);
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
