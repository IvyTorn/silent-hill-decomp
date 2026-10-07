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

    private Panel panel;
    private boolean started;
    private boolean listening;

    private static native byte[] nativePoll(int lastSerial);
    private static native void nativeInput(int kind, int a, int b);

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

        Log.i(TAG, "build: fase2 (touch)  config: enabled=" + enabled + " display=" + disp + " focusable=" + focusable
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
        int uiState;
        int uiSlot;
        int uiCmd;
        int eventSeq;
        int eventCode;
        final List<Item> items = new ArrayList<Item>();
    }

    private static final Charset LATIN1 = Charset.forName("ISO-8859-1");

    static Snapshot parse(byte[] b) {
        final int head = 4 + 24;
        if (b == null || b.length < head) return null;
        if (b[4] != 'S' || b[5] != 'H' || b[6] != '2' || b[7] != 'S' || b[8] != 2) return null;

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

        /* Everything is laid out in units of 1/1240 of the view's width when
         * the panel is wider than tall (the Thor's second screen reports
         * 1240x1080, so that is 1:1) and 1/1080 when it is taller than wide.
         * Any other panel scales instead of reflowing. */
        private float u = 1f;
        private boolean wide;

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

        private static final int ACT_CANCEL = -1, ACT_DISMISS = -2;
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
            if (lastEventSeq >= 0 && s.eventSeq != lastEventSeq) {
                showToast(eventText(s.eventCode));
            }
            lastEventSeq = s.eventSeq;
            if (s.uiState != UI_IDLE) {
                pendingItem = null;
            }
            snap = s;
            invalidate();
        }

        private void showToast(String t) {
            toast = t;
            toastUntil = android.os.SystemClock.uptimeMillis() + 2200;
            pendingItem = null;
            postInvalidateDelayed(2250);
        }

        private String eventText(int code) {
            switch (code) {
                case 2: return tr("No se puede usar aqu\u00ED", "Can't be used here");
                case 3: return tr("Demasiado oscuro para examinarlo", "Too dark to look at it");
                case 4: return tr("El juego no respondi\u00F3", "The game did not respond");
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

        @Override protected void onSizeChanged(int w, int h, int oldw, int oldh) {
            super.onSizeChanged(w, h, oldw, oldh);
            wide = w >= h;
            u = w / (wide ? 1240f : 1080f);
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
            switch (e.getActionMasked()) {
                case MotionEvent.ACTION_DOWN:
                    downX = e.getX();
                    downY = e.getY();
                    lastTouchY = downY;
                    dragging = false;
                    return true;

                case MotionEvent.ACTION_MOVE: {
                    /* A finger that travels is scrolling the list, not
                     * choosing from it. */
                    if (!dragging && Math.abs(e.getY() - downY) > 22 * u) {
                        dragging = true;
                        lastTouchY = e.getY();
                    }
                    if (dragging && sheetItem() == null) {
                        float dy = e.getY() - lastTouchY;
                        lastTouchY = e.getY();
                        float ns = Math.max(0f, Math.min(maxScroll, scrollY - dy));
                        if (ns != scrollY) {
                            scrollY = ns;
                            invalidate();
                        }
                    }
                    return true;
                }

                case MotionEvent.ACTION_UP:
                    if (!dragging && Math.abs(e.getX() - downX) <= 22 * u) {
                        handleTap(e.getX(), e.getY());
                    }
                    return true;

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

            final float gap = 12 * u;

            if (wide) {
                /* Wider than tall: the two header panels share one row, which
                 * leaves seven full rows of items on a 1240x1080 panel. */
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
             * which is what phase 2 needs for a touch target. */
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
            RectF br = buttonRects[buttonCount];
            br.set(x, y, x + w, y + h);
            buttonActions[buttonCount] = action;
            buttonCount++;

            fill.setColor(primary ? C_ACCENT : 0xFF2A2B30);
            c.drawRoundRect(br, 18 * u, 18 * u, fill);
            text.setTextAlign(Paint.Align.CENTER);
            text.setColor(primary ? 0xFF14110A : C_TEXT);
            text.setFakeBoldText(primary);
            float size = fit(label, (primary ? 60 : 48) * u * textScale, w - 40 * u, 30 * u);
            c.drawText(label, x + w / 2f, y + h / 2f + size * 0.35f, text);
            text.setFakeBoldText(false);
        }

        private void drawSheet(Canvas c, int w, int h, Item item) {
            int state = (snap.uiState == UI_IDLE) ? UI_OPENING : snap.uiState;

            fill.setColor(0xD9000000);
            r.set(0, 0, w, h);
            c.drawRect(r, fill);

            String[] labels = (state == UI_MENU) ? commandLabels(snap.uiCmd) : new String[0];
            String note = null;
            String closeLabel = null;
            int closeAction = ACT_CANCEL;

            if (state == UI_OPENING || state == UI_SEEK) {
                note = tr("Abriendo el inventario\u2026", "Opening the inventory\u2026");
                closeLabel = tr("Cancelar", "Cancel");
            } else if (state == UI_MENU) {
                if (labels.length == 0) {
                    note = tr("No se puede usar aqu\u00ED", "Can't be used here");
                    closeLabel = tr("Volver", "Back");
                } else {
                    closeLabel = tr("Cancelar", "Cancel");
                }
            } else if (state == UI_VIEWING) {
                note = tr("M\u00EDralo en la pantalla principal", "Look at the main screen");
                closeLabel = tr("Cerrar", "Close");
                closeAction = ACT_DISMISS;
            } else if (state == UI_CLOSING || state == UI_SETTLE) {
                note = tr("Volviendo al juego\u2026", "Returning to the game\u2026");
            } else {
                note = tr("Un momento\u2026", "One moment\u2026");
            }

            final float pw = Math.min(w - 120 * u, 960 * u);
            final float bh = 150 * u;
            final float bgap = 22 * u;
            float ph = 150 * u
                     + (note != null ? 80 * u : 0f)
                     + labels.length * (bh + bgap)
                     + (closeLabel != null ? (120 * u + bgap) : 0f)
                     + 30 * u;
            float px = (w - pw) / 2f;
            float py = Math.max(20 * u, (h - ph) / 2f);

            fill.setColor(0xFF1B1C21);
            r.set(px, py, px + pw, py + ph);
            c.drawRoundRect(r, 24 * u, 24 * u, fill);
            line.setColor(C_ACCENT);
            line.setStrokeWidth(3 * u);
            c.drawRoundRect(r, 24 * u, 24 * u, line);

            drawGlyph(c, item.id >> 5, px + 86 * u, py + 82 * u, 38 * u, groupColor(item.id >> 5));
            text.setTextAlign(Paint.Align.LEFT);
            text.setColor(C_TEXT);
            float nameMax = pw - 160 * u - 36 * u;
            fit(item.name, 60 * u * textScale, nameMax, 34 * u);
            c.drawText(ellipsize(item.name, nameMax), px + 160 * u, py + 102 * u, text);

            float y = py + 150 * u;

            if (note != null) {
                text.setTextAlign(Paint.Align.CENTER);
                text.setColor(C_DIM);
                fit(note, 42 * u * textScale, pw - 60 * u, 28 * u);
                c.drawText(note, px + pw / 2f, y + 34 * u, text);
                y += 80 * u;
            }

            for (int i = 0; i < labels.length; i++) {
                addButton(c, px + 36 * u, y, pw - 72 * u, bh, labels[i], i, true);
                y += bh + bgap;
            }

            if (closeLabel != null) {
                addButton(c, px + 36 * u, y, pw - 72 * u, 120 * u, closeLabel, closeAction, false);
            }
        }

        private void drawToast(Canvas c, int w, int h) {
            text.setTextAlign(Paint.Align.CENTER);
            float size = fit(toast, 46 * u * textScale, w - 160 * u, 28 * u);
            float tw = text.measureText(toast) + 80 * u;
            float th = 110 * u;
            float ty = h - th - 40 * u;
            fill.setColor(0xF2D43C3C);
            r.set((w - tw) / 2f, ty, (w + tw) / 2f, ty + th);
            c.drawRoundRect(r, 20 * u, 20 * u, fill);
            text.setColor(0xFFFFFFFF);
            c.drawText(toast, w / 2f, ty + th / 2f + size * 0.35f, text);
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
            text.setTextSize(30 * u * textScale);
            c.drawText(tr("ESTADO", "STATUS"), x + 28 * u, y + 46 * u, text);

            text.setTextAlign(Paint.Align.RIGHT);
            text.setColor(C_TEXT);
            text.setTextSize(48 * u * textScale);
            String pctText = Math.round(pct) + "%";
            float pctW = text.measureText(pctText);
            c.drawText(pctText, x + w - 28 * u, y + 100 * u, text);

            text.setTextAlign(Paint.Align.LEFT);
            text.setColor(col);
            text.setFakeBoldText(true);
            fit(word, 52 * u * textScale, w - 56 * u - pctW - 20 * u, 30 * u);
            c.drawText(word, x + 28 * u, y + 100 * u, text);
            text.setFakeBoldText(false);

            float bx = x + 28 * u;
            float bw = w - 56 * u;
            float by = y + 116 * u;
            float bh = 20 * u;
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
            float ammoW = 0f;

            if (gun) {
                text.setTextAlign(Paint.Align.RIGHT);
                text.setColor(C_DIM);
                text.setTextSize(40 * u * textScale);
                String reserve = " / " + snap.ammoReserve;
                float rw = text.measureText(reserve);
                c.drawText(reserve, x + w - 28 * u, y + 106 * u, text);
                text.setColor(snap.ammoLoaded == 0 ? 0xFFD43C3C : C_TEXT);
                text.setFakeBoldText(true);
                text.setTextSize(76 * u * textScale);
                String loaded = String.valueOf(snap.ammoLoaded);
                ammoW = rw + text.measureText(loaded) + 24 * u;
                c.drawText(loaded, x + w - 28 * u - rw, y + 110 * u, text);
                text.setFakeBoldText(false);
            }

            float nameMax = w - 150 * u - 28 * u - ammoW;
            text.setTextAlign(Paint.Align.LEFT);
            text.setColor(C_TEXT);
            fit(eq.name, 52 * u * textScale, nameMax, 32 * u);
            c.drawText(ellipsize(eq.name, nameMax), x + 150 * u, y + 108 * u, text);
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
