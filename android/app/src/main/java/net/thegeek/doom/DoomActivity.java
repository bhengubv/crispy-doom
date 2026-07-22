package net.thegeek.doom;

import android.content.pm.ActivityInfo;
import android.content.Context;
import android.media.AudioManager;
import android.net.wifi.WifiManager;
import android.net.Uri;
import android.os.Bundle;
import java.io.File;
import java.io.FileOutputStream;
import java.io.InputStream;
import java.util.ArrayList;
import org.libsdl.app.SDLActivity;

// Native Crispy Doom for Circle OS. Extracts the bundled freedoom IWAD to the
// app's files dir (crispy finds it via DOOMWADDIR, set in the manifest), then
// hands off to SDLActivity which loads libmain.so -> SDL_main == crispy main().
public class DoomActivity extends SDLActivity {

    private WifiManager.MulticastLock multicastLock;

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        // DOOM is a landscape game; in portrait crispy's widescreen sizing runs
        // past the column buffer (R_DrawColumn range error). Force it here so we
        // don't depend on the manifest being honoured by the launcher/OEM skin.
        setRequestedOrientation(ActivityInfo.SCREEN_ORIENTATION_SENSOR_LANDSCAPE);
        // Route the hardware volume rocker to the game's audio (the media
        // stream, where SDL plays). Without this the rocker adjusts the ring
        // stream by default, so pressing it does nothing to DOOM's volume.
        setVolumeControlStream(AudioManager.STREAM_MUSIC);
        // Finding a host on the LAN means listening for a broadcast, and on
        // several Android versions the wifi stack drops those unless something
        // is holding a multicast lock. Held for the life of the activity: it
        // costs a little battery and buys netgame discovery working at all.
        try {
            WifiManager wifi = (WifiManager)
                getApplicationContext().getSystemService(Context.WIFI_SERVICE);
            if (wifi != null) {
                multicastLock = wifi.createMulticastLock("doom-netgame");
                multicastLock.setReferenceCounted(false);
                multicastLock.acquire();
            }
        } catch (Exception e) {
            e.printStackTrace();
        }
        try {
            extractAsset("freedoom2.wad");
        } catch (Exception e) {
            e.printStackTrace();
        }
        super.onCreate(savedInstanceState);
    }

    private void extractAsset(String name) throws Exception {
        File out = new File(getFilesDir(), name);
        if (out.exists() && out.length() > 1000000L) {
            return; // already extracted
        }
        InputStream in = getAssets().open(name);
        FileOutputStream fos = new FileOutputStream(out);
        byte[] buf = new byte[65536];
        int n;
        while ((n = in.read(buf)) > 0) {
            fos.write(buf, 0, n);
        }
        fos.close();
        in.close();
    }

    // Settings live in the Arcade so DOOM can also ship standalone. The Arcade
    // launches us with circledoom://play?ghost=1&opacity=35 and we translate
    // the query into argv, which SDLActivity hands to SDL_main.
    @Override
    protected String[] getArguments() {
        ArrayList<String> args = new ArrayList<String>();
        try {
            Uri data = getIntent() != null ? getIntent().getData() : null;
            if (data != null) {
                String ghost = data.getQueryParameter("ghost");
                if (ghost != null && ghost.length() > 0) {
                    args.add("-ghost");
                    args.add(ghost);
                }
                String opacity = data.getQueryParameter("opacity");
                if (opacity != null && opacity.length() > 0) {
                    args.add("-ghostalpha");
                    args.add(opacity);
                }
                String shader = data.getQueryParameter("shader");
                if (shader != null && shader.length() > 0) {
                    args.add("-shader");
                    args.add(shader);
                }
            }
        } catch (Exception e) {
            e.printStackTrace(); // never let a malformed intent stop the game
        }
        return args.toArray(new String[0]);
    }

    @Override
    protected String[] getLibraries() {
        return new String[] { "SDL2", "SDL2_mixer", "SDL2_net", "main" };
    }
}
