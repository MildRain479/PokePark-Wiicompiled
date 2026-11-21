package io.github.pokeparkrecomp;

import android.app.ActivityOptions;
import android.app.AlertDialog;
import android.app.NativeActivity;
import android.app.Presentation;
import android.content.Context;
import android.content.Intent;
import android.content.SharedPreferences;
import android.content.pm.PackageManager;
import android.database.Cursor;
import android.hardware.display.DisplayManager;
import android.net.Uri;
import android.os.Bundle;
import android.os.Handler;
import android.os.Looper;
import android.os.ParcelFileDescriptor;
import android.provider.DocumentsContract;
import android.provider.OpenableColumns;
import android.util.Log;
import android.view.Display;
import android.view.InputDevice;
import android.view.MotionEvent;
import android.view.Surface;
import android.view.SurfaceHolder;
import android.view.SurfaceView;
import android.view.WindowManager;

import java.io.ByteArrayOutputStream;
import java.io.InputStream;
import java.net.HttpURLConnection;
import java.net.URL;
import java.util.ArrayList;

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
 * there. And for bringing over a save (save_import_android.c): asking for
 * its files, and starting the game again once they're in place.
 */
public class PokeParkActivity extends NativeActivity {
    private static final int PICK = 1;
    private static final int PICK_FOLDER = 2;
    private static final int PICK_SAVE = 3;
    private static final int PICK_EXPORT = 4;
    private static final int PICK_LANGUAGE = 5;
    private static final String DISC = "disc";
    private static final String PHOTOS = "photos";
    private volatile int pickedFd = -1;
    private volatile int pickedFolder = -1;
    private volatile int pickedSave = -1;
    /** This process asked for a save (chooseSave). */
    private boolean askedSave;
    /** The files chosen came to a game started again, not the one that
     * asked: Android stopped that one (for its memory, say) while the
     * picker was up, and started it again to give it the answer. */
    private volatile boolean savePending;
    private final ArrayList<Uri> saveFiles = new ArrayList<>();
    /** Where the save's zip goes: -2 while asking, then a file descriptor
     * open for writing, or -1 (exportPending as savePending). */
    private volatile int exportFd = -1;
    private boolean askedExport;
    private volatile boolean exportPending;
    private String asking;
    /** The disc of another language for its pack (langpack_android.c):
     * -2 while asking, then a file descriptor open for reading, or -1. */
    private volatile int languageFd = -1;

    private SharedPreferences prefs() {
        return getSharedPreferences("pokepark", MODE_PRIVATE);
    }

    private int open(Uri uri) {
        return open(uri, "r");
    }

    private int open(Uri uri, String mode) {
        try {
            ParcelFileDescriptor pfd = getContentResolver().openFileDescriptor(uri, mode);
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
    /** The second screen is on in the settings (native code asked for it). */
    private boolean secondWanted;
    /** The game is on screen (between onStart and onStop). */
    private boolean visible;

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
     * built-in one, or else a touch screen plugged in. Not the one the game
     * itself is on. */
    private Display secondDisplay() {
        DisplayManager dm = (DisplayManager) getSystemService(Context.DISPLAY_SERVICE);
        Display[] all = dm.getDisplays(DisplayManager.DISPLAY_CATEGORY_PRESENTATION);
        Display mine = getDisplay();
        int own = mine == null ? Display.DEFAULT_DISPLAY : mine.getDisplayId();
        for (Display d : all) {
            if (d.getDisplayId() != own && builtIn(d)) return d;
        }
        for (Display d : all) {
            if (d.getDisplayId() != own && touchScreenPluggedIn(d)) return d;
        }
        return null;
    }

    private static final String MOVED = "moved";

    /** Started on a handheld's lower screen (the AYN Thor's own launcher
     * there, or its app switcher), the game goes to the main one, leaving
     * the lower one for the second screen: started again there, in a new
     * process, as restartGame does, before NativeActivity starts the
     * game. Once only, should it not get there. */
    @Override
    protected void onCreate(Bundle state) {
        Display mine = getDisplay();
        if (mine != null && mine.getDisplayId() != Display.DEFAULT_DISPLAY && builtIn(mine)
                && !getIntent().getBooleanExtra(MOVED, false)) {
            Intent start = Intent.makeRestartActivityTask(getComponentName()).putExtra(MOVED, true);
            ActivityOptions to = ActivityOptions.makeBasic().setLaunchDisplayId(Display.DEFAULT_DISPLAY);
            Log.i("PokePark", "started on display " + mine.getDisplayId() + ": moving to the main one");
            startActivity(start, to.toBundle());
            android.os.Process.killProcess(android.os.Process.myPid());
            return;
        }
        super.onCreate(state);
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
     * code, once its methods above are registered). It's there while the
     * game is: closed when it's left (Home, or another app opened over it)
     * and opened again on coming back. */
    public void showSecondScreen() {
        runOnUiThread(() -> {
            secondWanted = true;
            openSecond();
        });
    }

    private void openSecond() {
        if (second != null || !visible || !secondWanted) return;
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
        /* Never focused: touched, it would take the keys, the controller's
         * among them, from the game on the main screen (with the AYN Thor's
         * focus lock on, it kept them until the main screen was touched). */
        /* Not closed by Back pressed on it; and should something else
         * close it, it opens again. */
        second.setCancelable(false);
        final Presentation mine = second;
        second.setOnDismissListener(d -> {
            if (second != mine) return; /* ours, on purpose */
            second = null;
            new Handler(Looper.getMainLooper()).postDelayed(this::openSecond, 500);
        });
        try {
            second.show();
            second.getWindow().addFlags(WindowManager.LayoutParams.FLAG_NOT_FOCUSABLE);
        } catch (Exception e) {
            Log.w("PokePark", "no second screen: " + e);
            second = null;
        }
    }

    private void closeSecond() {
        Presentation p = second;
        second = null;
        if (p != null) p.dismiss();
    }

    /** Closes the second screen's window, if open (from native code). */
    public void hideSecondScreen() {
        runOnUiThread(() -> {
            secondWanted = false;
            closeSecond();
        });
    }

    @Override
    protected void onStart() {
        super.onStart();
        visible = true;
        openSecond();
    }

    @Override
    protected void onStop() {
        visible = false;
        closeSecond();
        super.onStop();
    }

    @Override
    protected void onDestroy() {
        if (displays != null) ((DisplayManager) getSystemService(Context.DISPLAY_SERVICE)).unregisterDisplayListener(displays);
        closeSecond();
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

    /** Says why the game can't go on, with only a way to close it (from
     * native code, any thread). */
    public void fatal(final String message) {
        runOnUiThread(() -> {
            hideSecondScreen();
            new AlertDialog.Builder(this, android.R.style.Theme_DeviceDefault_Dialog_Alert)
                .setTitle("PokePark")
                .setMessage(message)
                .setCancelable(false)
                .setPositiveButton("Close", (d, w) -> {
                    finishAndRemoveTask();
                    android.os.Process.killProcess(android.os.Process.myPid());
                })
                .show();
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

    /** Asks for a save to bring over: a Wii's data.bin, or the files of
     * Dolphin's save folder, several at once, or a zip of them (the
     * picker can't choose a folder) (from native code; takeSaveCount
     * answers). */
    public void chooseSave() {
        pickedSave = -1;
        askedSave = true;
        runOnUiThread(() -> {
            Intent pick = new Intent(Intent.ACTION_OPEN_DOCUMENT);
            pick.addCategory(Intent.CATEGORY_OPENABLE);
            pick.setType("*/*");
            pick.putExtra(Intent.EXTRA_ALLOW_MULTIPLE, true);
            try {
                startActivityForResult(pick, PICK_SAVE);
            } catch (Exception e) {
                Log.w("PokePark", "no file picker", e);
                pickedSave = 0;
            }
        });
    }

    /** -1 while asking, then how many files were chosen (polled from
     * native code). */
    public int takeSaveCount() {
        return pickedSave;
    }

    /** Asks where to put the save's zip (from native code; takeExportFd
     * answers). */
    public void chooseExport() {
        exportFd = -2;
        askedExport = true;
        runOnUiThread(() -> {
            Intent pick = new Intent(Intent.ACTION_CREATE_DOCUMENT);
            pick.addCategory(Intent.CATEGORY_OPENABLE);
            pick.setType("application/zip");
            pick.putExtra(Intent.EXTRA_TITLE, "PokePark Wii save.zip");
            try {
                startActivityForResult(pick, PICK_EXPORT);
            } catch (Exception e) {
                Log.w("PokePark", "no file picker", e);
                exportFd = -1;
            }
        });
    }

    /** Asks for a disc image of another language, for its pack (from
     * native code; takeLanguageFd answers). */
    public void chooseLanguageDisc() {
        languageFd = -2;
        runOnUiThread(() -> {
            Intent pick = new Intent(Intent.ACTION_OPEN_DOCUMENT);
            pick.addCategory(Intent.CATEGORY_OPENABLE);
            pick.setType("*/*");
            try {
                startActivityForResult(pick, PICK_LANGUAGE);
            } catch (Exception e) {
                Log.w("PokePark", "no file picker", e);
                languageFd = -1;
            }
        });
    }

    /** -2 while asking, then the disc image, open for reading, or -1
     * (polled from native code). */
    public int takeLanguageFd() {
        int fd = languageFd;
        if (fd != -2) languageFd = -1;
        return fd;
    }

    /** -2 while asking, then the file for the zip, open for writing, or -1
     * (polled from native code). */
    public int takeExportFd() {
        return exportFd;
    }

    /** The file for a zip asked for by a game since stopped, once, -2 while
     * it's being opened, or -1 (from native code, as the game starts:
     * save_import_pending). */
    public int takePendingExport() {
        if (!exportPending) return -1;
        if (exportFd == -2) return -2;
        exportPending = false;
        int fd = exportFd;
        exportFd = -1;
        return fd;
    }

    /** How many files were chosen for a game since stopped (savePending),
     * once, or 0 (from native code, as the game starts, before it reads
     * its saves: save_import_pending). */
    public int takePendingSaves() {
        if (!savePending) return 0;
        savePending = false;
        return saveFiles.size();
    }

    /** The name of chosen file i (from native code). */
    public String saveName(int i) {
        try (Cursor c = getContentResolver().query(saveFiles.get(i),
                new String[] {OpenableColumns.DISPLAY_NAME}, null, null, null)) {
            if (c != null && c.moveToFirst() && c.getString(0) != null) return c.getString(0);
        } catch (Exception e) {
            Log.w("PokePark", "no name for " + saveFiles.get(i), e);
        }
        return "";
    }

    /** Chosen file i, open for reading, or -1 (from native code). */
    public int openSave(int i) {
        return open(saveFiles.get(i));
    }

    /** Starts the game again, in a new process (from native code). On the
     * UI thread, and the process killed outright: exit() from the calling
     * thread ran the native libraries' teardown while the game waited on
     * that thread, and the activity, going, waited on the game for its
     * window, so it hung ("isn't responding") after an import. */
    public void restartGame() {
        Intent start = getPackageManager().getLaunchIntentForPackage(getPackageName());
        if (start == null) return;
        runOnUiThread(() -> {
            startActivity(Intent.makeRestartActivityTask(start.getComponent()));
            android.os.Process.killProcess(android.os.Process.myPid());
        });
    }

    @Override
    protected void onActivityResult(int request, int result, Intent data) {
        if (request == PICK_SAVE) {
            saveFiles.clear();
            if (result == RESULT_OK && data != null) {
                if (data.getClipData() != null)
                    for (int i = 0; i < data.getClipData().getItemCount(); i++)
                        saveFiles.add(data.getClipData().getItemAt(i).getUri());
                else if (data.getData() != null)
                    saveFiles.add(data.getData());
            }
            pickedSave = saveFiles.size();
            if (!askedSave && !saveFiles.isEmpty()) savePending = true;
            return;
        }
        if (request == PICK_LANGUAGE) {
            Uri uri = result == RESULT_OK && data != null ? data.getData() : null;
            if (uri == null) {
                languageFd = -1;
                return;
            }
            new Thread(() -> languageFd = open(uri), "open language disc").start();
            return;
        }
        if (request == PICK_EXPORT) {
            Uri uri = result == RESULT_OK && data != null ? data.getData() : null;
            if (uri == null) {
                exportFd = -1;
                return;
            }
            exportFd = -2;
            if (!askedExport) exportPending = true;
            /* Opened on a thread of its own, as a cloud service's may be slow. */
            new Thread(() -> exportFd = open(uri, "wt"), "open save export").start();
            return;
        }
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
        /* Opened on a thread of its own: a file from a cloud service (Google
         * Drive's, say) is downloaded as it's opened, a few GB, and on this
         * one the app would stop answering ("isn't responding"). */
        new Thread(() -> {
            try {
                getContentResolver().takePersistableUriPermission(uri, Intent.FLAG_GRANT_READ_URI_PERMISSION);
            } catch (Exception e) {
                Log.w("PokePark", "no lasting permission for " + uri, e);
            }
            int fd = open(uri);
            if (fd < 0) {
                runOnUiThread(() -> ask("That file couldn't be opened. Choose your disc image of PokePark Wii again."));
                return;
            }
            prefs().edit().putString(DISC, uri.toString()).apply();
            pickedFd = fd;
        }, "open disc image").start();
    }
}
