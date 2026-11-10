package io.github.pokeparkrecomp;

import android.app.AlertDialog;
import android.app.NativeActivity;
import android.app.Presentation;
import android.content.Context;
import android.content.Intent;
import android.content.SharedPreferences;
import android.content.pm.PackageManager;
import android.hardware.display.DisplayManager;
import android.net.Uri;
import android.os.Handler;
import android.os.Looper;
import android.os.ParcelFileDescriptor;
import android.provider.DocumentsContract;
import android.util.Log;
import android.view.Display;
import android.view.InputDevice;
import android.view.MotionEvent;
import android.view.Surface;
import android.view.SurfaceHolder;
import android.view.SurfaceView;

import java.io.ByteArrayOutputStream;
import java.io.InputStream;
import java.net.HttpURLConnection;
import java.net.URL;

/**
 * The app's activity: NativeActivity (window_android.c runs the game), and
 * the one thing it can't do, asking for the player's disc image. The file
 * picker's answer comes back to an activity, so this asks, keeps a lasting
 * permission to read the file, remembers it, and hands the native side an
 * open file descriptor (disc_image_android.c reads the game from it).
 * And for the update check (update.c): fetching the latest release's
 * details, and opening its page in the browser. And on a device with a
 * second screen (the AYN Thor's lower one, or a touch screen plugged in, as
 * Retroid's Dual Screen Add-On), a window there whose surface and touches
 * go to the native side (second_screen_android.c), shown and hidden as its
 * settings say. And for the photos the game copies to its SD card
 * (sdcard_android.c): asking for a folder for them, and making each one
 * there.
 */
public class PokeParkActivity extends NativeActivity {
    private static final int PICK = 1;
    private static final int PICK_FOLDER = 2;
    private static final String DISC = "disc";
    private static final String PHOTOS = "photos";
    private volatile int pickedFd = -1;
    private volatile int pickedFolder = -1;
    private String asking;

    private SharedPreferences prefs() {
        return getSharedPreferences("pokepark", MODE_PRIVATE);
    }

    private int open(Uri uri) {
        try {
            ParcelFileDescriptor pfd = getContentResolver().openFileDescriptor(uri, "r");
            return pfd == null ? -1 : pfd.detachFd();
        } catch (Exception e) {
            Log.w("PokePark", "can't open " + uri, e);
            return -1;
        }
    }

    /** The disc image chosen before, opened, or -1 (from native code). */
    public int savedGameFd() {
        String uri = prefs().getString(DISC, null);
        return uri == null ? -1 : open(Uri.parse(uri));
    }

    /** It wasn't the game after all (from native code). */
    public void forgetGame() {
        prefs().edit().remove(DISC).apply();
    }

    /** The disc image just chosen, once, or -1 (polled from native code). */
    public int takePickedFd() {
        int fd = pickedFd;
        pickedFd = -1;
        return fd;
    }

    /** An HTTPS GET's body, or null (from native code, off the main thread). */
    public String httpGet(String url, String userAgent) {
        HttpURLConnection c = null;
        try {
            c = (HttpURLConnection) new URL(url).openConnection();
            c.setConnectTimeout(10000);
            c.setReadTimeout(10000);
            c.setRequestProperty("Accept", "application/vnd.github+json");
            c.setRequestProperty("User-Agent", userAgent);
            if (c.getResponseCode() != 200) return null;
            InputStream in = c.getInputStream();
            ByteArrayOutputStream out = new ByteArrayOutputStream();
            byte[] buf = new byte[8192];
            for (int n; out.size() < 256 * 1024 && (n = in.read(buf)) > 0;) out.write(buf, 0, n);
            in.close();
            return out.toString("UTF-8");
        } catch (Exception e) {
            Log.w("PokePark", "update check: " + e);
            return null;
        } finally {
            if (c != null) c.disconnect();
        }
    }

    /** The second screen's surface, or null when it goes, and its size. */
    private static native void secondSurface(Surface surface, int width, int height);
    /** A touch on it: 0 down, 1 up, 2 moved, of pointer id, at x, y in its
     * pixels. */
    private static native void secondTouch(int action, int id, float x, float y);
    /** Whether this is a handheld with two screens, again: one was plugged
     * in or out. */
    private static native void secondDevice(boolean dual);

    private Presentation second;
    private DisplayManager.DisplayListener displays;

    /** Whether a display is built in (not HDMI, wireless or virtual):
     * Display.getType, hidden, is TYPE_INTERNAL (1). Unknown is not. */
    private static boolean builtIn(Display d) {
        try {
            return (Integer) Display.class.getMethod("getType").invoke(d) == 1;
        } catch (Exception e) {
            return false;
        }
    }

    /** Whether a display is a touch screen plugged in, as Retroid's Dual
     * Screen Add-On (over USB-C, so to Android an external display, as a TV
     * or a monitor would be): it has a touch screen of its own, one Android
     * gives it (InputDevice.getAssociatedDisplayId, hidden), or else one
     * plugged in too. A TV or a monitor without one isn't. */
    private static boolean touchScreenPluggedIn(Display d) {
        try {
            if ((Integer) Display.class.getMethod("getType").invoke(d) != 2) return false; /* TYPE_EXTERNAL */
        } catch (Exception e) {
            return false;
        }
        for (int id : InputDevice.getDeviceIds()) {
            InputDevice in = InputDevice.getDevice(id);
            if (in == null || (in.getSources() & InputDevice.SOURCE_TOUCHSCREEN) != InputDevice.SOURCE_TOUCHSCREEN) continue;
            int on = -1;
            try {
                on = (Integer) InputDevice.class.getMethod("getAssociatedDisplayId").invoke(in);
            } catch (Exception e) {
                /* not asked: taken as unknown */
            }
            if (on == d.getDisplayId() || (on == -1 && in.isExternal())) return true;
        }
        return false;
    }

    /** The second screen to show on, of those apps can present on: a
     * built-in one, or else a touch screen plugged in. */
    private Display secondDisplay() {
        DisplayManager dm = (DisplayManager) getSystemService(Context.DISPLAY_SERVICE);
        Display[] all = dm.getDisplays(DisplayManager.DISPLAY_CATEGORY_PRESENTATION);
        for (Display d : all) {
            if (builtIn(d)) return d;
        }
        for (Display d : all) {
            if (touchScreenPluggedIn(d)) return d;
        }
        return null;
    }

    private boolean dual() {
        boolean pad = false;
        if (getPackageManager().hasSystemFeature(PackageManager.FEATURE_TELEPHONY)) return false;
        for (int id : InputDevice.getDeviceIds()) {
            InputDevice d = InputDevice.getDevice(id);
            if (d != null && (d.getSources() & InputDevice.SOURCE_GAMEPAD) == InputDevice.SOURCE_GAMEPAD) pad = true;
        }
        boolean dual = pad && secondDisplay() != null;
        Log.i("PokePark", "dual-screen handheld: " + dual + (pad ? "" : " (no controller)"));
        return dual;
    }

    /** Whether this is a handheld with two screens, as the AYN Thor, or one
     * with Retroid's Dual Screen Add-On on (from native code): a second
     * screen, built in or a touch screen plugged in, a controller, and no
     * phone in it. Not a PC, a handheld with one screen, or a phone. Asked
     * again whenever a screen is plugged in or out. */
    public boolean dualScreenHandheld() {
        runOnUiThread(() -> {
            if (displays != null) return;
            displays = new DisplayManager.DisplayListener() {
                public void onDisplayAdded(int id) {
                    secondDevice(dual());
                }
                public void onDisplayRemoved(int id) {
                    /* Its window went with it. */
                    if (second != null && second.getDisplay().getDisplayId() == id) second = null;
                    secondDevice(dual());
                }
                public void onDisplayChanged(int id) {}
            };
            ((DisplayManager) getSystemService(Context.DISPLAY_SERVICE))
                .registerDisplayListener(displays, new Handler(Looper.getMainLooper()));
        });
        return dual();
    }

    /** Opens a window on a second screen, if there is one (from native
     * code, once its methods above are registered). */
    public void showSecondScreen() {
        runOnUiThread(() -> {
            if (second != null) return;
            Display display = secondDisplay();
            if (display == null) return;
            second = new Presentation(this, display);
            SurfaceView view = new SurfaceView(second.getContext());
            view.getHolder().addCallback(new SurfaceHolder.Callback() {
                public void surfaceCreated(SurfaceHolder h) {}
                public void surfaceChanged(SurfaceHolder h, int format, int w, int hgt) {
                    secondSurface(h.getSurface(), w, hgt);
                }
                public void surfaceDestroyed(SurfaceHolder h) {
                    secondSurface(null, 0, 0);
                }
            });
            view.setOnTouchListener((v, e) -> {
                int a = e.getActionMasked(), p = e.getActionIndex();
                if (a == MotionEvent.ACTION_DOWN || a == MotionEvent.ACTION_POINTER_DOWN) {
                    secondTouch(0, e.getPointerId(p), e.getX(p), e.getY(p));
                } else if (a == MotionEvent.ACTION_UP || a == MotionEvent.ACTION_POINTER_UP) {
                    secondTouch(1, e.getPointerId(p), e.getX(p), e.getY(p));
                } else if (a == MotionEvent.ACTION_MOVE || a == MotionEvent.ACTION_CANCEL) {
                    for (int i = 0; i < e.getPointerCount(); i++)
                        secondTouch(a == MotionEvent.ACTION_MOVE ? 2 : 1, e.getPointerId(i), e.getX(i), e.getY(i));
                }
                return true;
            });
            second.setContentView(view);
            try {
                second.show();
            } catch (Exception e) {
                Log.w("PokePark", "no second screen: " + e);
                second = null;
            }
        });
    }

    /** Closes the second screen's window, if open (from native code). */
    public void hideSecondScreen() {
        runOnUiThread(() -> {
            if (second != null) second.dismiss();
            second = null;
        });
    }

    @Override
    protected void onDestroy() {
        if (displays != null) ((DisplayManager) getSystemService(Context.DISPLAY_SERVICE)).unregisterDisplayListener(displays);
        if (second != null) second.dismiss();
        second = null;
        super.onDestroy();
    }

    /** Opens url in the browser (from native code). */
    public void openPage(final String url) {
        runOnUiThread(() -> {
            try {
                startActivity(new Intent(Intent.ACTION_VIEW, Uri.parse(url)));
            } catch (Exception e) {
                Log.w("PokePark", "can't open " + url, e);
            }
        });
    }

    /** Asks for the disc image, saying message (from native code). */
    public void chooseGame(final String message) {
        runOnUiThread(new Runnable() {
            public void run() {
                ask(message);
            }
        });
    }

    private void ask(String message) {
        asking = message;
        new AlertDialog.Builder(this, android.R.style.Theme_DeviceDefault_Dialog_Alert)
            .setTitle("PokePark")
            .setMessage(message)
            .setCancelable(false)
            .setPositiveButton("Choose disc image", (d, w) -> {
                Intent pick = new Intent(Intent.ACTION_OPEN_DOCUMENT);
                pick.addCategory(Intent.CATEGORY_OPENABLE);
                pick.setType("*/*");
                startActivityForResult(pick, PICK);
            })
            .setNegativeButton("Quit", (d, w) -> finish())
            .show();
    }

    /** Asks for a folder for the photos copied to the SD card, starting at
     * the one chosen last (from native code; takePhotoFolder answers). */
    public void choosePhotoFolder() {
        pickedFolder = -1;
        runOnUiThread(() -> {
            Intent pick = new Intent(Intent.ACTION_OPEN_DOCUMENT_TREE);
            String last = prefs().getString(PHOTOS, null);
            if (last != null) pick.putExtra(DocumentsContract.EXTRA_INITIAL_URI, Uri.parse(last));
            try {
                startActivityForResult(pick, PICK_FOLDER);
            } catch (Exception e) {
                Log.w("PokePark", "no folder picker", e);
                pickedFolder = 0;
            }
        });
    }

    /** -1 while asking, then 1 if a folder was chosen or 0 if not (polled
     * from native code). */
    public int takePhotoFolder() {
        return pickedFolder;
    }

    /** A new file named name in the photos' folder, open for writing, or
     * -1 (from native code). A name that's taken gets a number added. */
    public int createPhoto(String name) {
        try {
            Uri tree = Uri.parse(prefs().getString(PHOTOS, ""));
            Uri dir = DocumentsContract.buildDocumentUriUsingTree(tree, DocumentsContract.getTreeDocumentId(tree));
            Uri doc = DocumentsContract.createDocument(getContentResolver(), dir, "image/jpeg", name);
            ParcelFileDescriptor pfd = doc == null ? null : getContentResolver().openFileDescriptor(doc, "w");
            return pfd == null ? -1 : pfd.detachFd();
        } catch (Exception e) {
            Log.w("PokePark", "can't save " + name, e);
            return -1;
        }
    }

    @Override
    protected void onActivityResult(int request, int result, Intent data) {
        if (request == PICK_FOLDER) {
            Uri uri = result == RESULT_OK && data != null ? data.getData() : null;
            if (uri != null) {
                try {
                    getContentResolver().takePersistableUriPermission(uri,
                        Intent.FLAG_GRANT_READ_URI_PERMISSION | Intent.FLAG_GRANT_WRITE_URI_PERMISSION);
                } catch (Exception e) {
                    Log.w("PokePark", "no lasting permission for " + uri, e);
                }
                prefs().edit().putString(PHOTOS, uri.toString()).apply();
            }
            pickedFolder = uri != null ? 1 : 0;
            return;
        }
        if (request != PICK) {
            super.onActivityResult(request, result, data);
            return;
        }
        Uri uri = result == RESULT_OK && data != null ? data.getData() : null;
        if (uri == null) {
            ask(asking); // backed out of the picker: there's nothing to play yet
            return;
        }
        try {
            getContentResolver().takePersistableUriPermission(uri, Intent.FLAG_GRANT_READ_URI_PERMISSION);
        } catch (Exception e) {
            Log.w("PokePark", "no lasting permission for " + uri, e);
        }
        int fd = open(uri);
        if (fd < 0) {
            ask("That file couldn't be opened. Choose your disc image of PokePark Wii again.");
            return;
        }
        prefs().edit().putString(DISC, uri.toString()).apply();
        pickedFd = fd;
    }
}
