package com.deivid.forza;

import android.Manifest;
import android.app.AlertDialog;
import android.content.Intent;
import android.content.pm.PackageManager;
import android.net.Uri;
import android.os.Build;
import android.os.Bundle;
import android.os.Environment;
import android.provider.Settings;
import android.view.View;
import android.view.WindowInsets;
import android.view.WindowInsetsController;
import android.widget.Toast;

import java.io.File;
import java.util.Locale;

import org.libsdl.app.SDLActivity;

// SDL's activity running libmain.so. The game goes in the Forza folder on the
// phone's storage (/storage/emulated/0/Forza) or at the top of an SD card.
// Reading it needs all files access (Android 11+) or storage permission
// (Android 10). No game asset is bundled in the APK.
public class ForzaActivity extends SDLActivity {
    private static final String SHARED = "Forza";
    private static final int REQ_STORAGE = 1;

    @Override
    protected String[] getLibraries() {
        return new String[]{"SDL3", "main"};
    }

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);
        hideSystemBars();
        if (BuildConfig.STUB) {
            new AlertDialog.Builder(this)
                    .setTitle("Stub build")
                    .setMessage("This APK was built without the game code.\n\n"
                            + "Set the FORZA_XBE_URL secret in the GitHub repository and run the build "
                            + "workflow again. See the README for the other secrets.")
                    .setPositiveButton("OK", null)
                    .show();
        } else {
            askForGame(true);
        }
    }

    @Override
    protected void onResume() {
        super.onResume();
        if (!BuildConfig.STUB) askForGame(false);
    }

    private void hideSystemBars() {
        if (Build.VERSION.SDK_INT >= 30) {
            WindowInsetsController c = getWindow().getInsetsController();
            if (c != null) {
                c.hide(WindowInsets.Type.systemBars());
                c.setSystemBarsBehavior(WindowInsetsController.BEHAVIOR_SHOW_TRANSIENT_BARS_BY_SWIPE);
            }
        } else {
            getWindow().getDecorView().setSystemUiVisibility(
                    View.SYSTEM_UI_FLAG_IMMERSIVE_STICKY
                            | View.SYSTEM_UI_FLAG_FULLSCREEN
                            | View.SYSTEM_UI_FLAG_HIDE_NAVIGATION
                            | View.SYSTEM_UI_FLAG_LAYOUT_FULLSCREEN
                            | View.SYSTEM_UI_FLAG_LAYOUT_HIDE_NAVIGATION
                            | View.SYSTEM_UI_FLAG_LAYOUT_STABLE);
        }
    }

    private File sharedDir() {
        return new File(Environment.getExternalStorageDirectory(), SHARED);
    }

    private boolean hasGame(File dir) {
        if (new File(dir, "default.xbe").isFile()) return true;
        File[] files = dir.listFiles();
        if (files != null) {
            for (File f : files) {
                if (f.isFile() && f.getName().toLowerCase(Locale.ROOT).endsWith(".iso")) return true;
            }
        }
        return false;
    }

    private void askForGame(boolean dialog) {
        File dir = sharedDir();
        if (!dir.exists() && !dir.mkdirs()) {
            // reported by the native side as well
        }
        if (hasGame(dir)) return;

        if (dialog && Build.VERSION.SDK_INT >= 30 && !Environment.isExternalStorageManager()) {
            new AlertDialog.Builder(this)
                    .setTitle("Game files")
                    .setMessage("Forza needs access to " + dir + ".\n\n"
                            + "Copy default.xbe and its media folder there (or the game ISO), then grant "
                            + "all-files access.")
                    .setPositiveButton("Grant access", (d, w) -> {
                        try {
                            startActivity(new Intent(
                                    Settings.ACTION_MANAGE_APP_ALL_FILES_ACCESS_PERMISSION,
                                    Uri.parse("package:" + getPackageName())));
                        } catch (Exception e) {
                            startActivity(new Intent(Settings.ACTION_MANAGE_ALL_FILES_ACCESS_PERMISSION));
                        }
                    })
                    .setNegativeButton("Not now", null)
                    .show();
            return;
        }

        if (dialog && Build.VERSION.SDK_INT < 30
                && checkSelfPermission(Manifest.permission.READ_EXTERNAL_STORAGE)
                != PackageManager.PERMISSION_GRANTED) {
            requestPermissions(new String[]{
                    Manifest.permission.READ_EXTERNAL_STORAGE,
                    Manifest.permission.WRITE_EXTERNAL_STORAGE}, REQ_STORAGE);
            return;
        }

        Toast.makeText(this, "Waiting for game files in " + dir, Toast.LENGTH_LONG).show();
    }

    @Override
    public void onRequestPermissionsResult(int requestCode, String[] permissions, int[] results) {
        super.onRequestPermissionsResult(requestCode, permissions, results);
        if (requestCode == REQ_STORAGE) askForGame(false);
    }
}
