package net.thegeek.doom;

import android.content.pm.ActivityInfo;
import android.os.Bundle;
import java.io.File;
import java.io.FileOutputStream;
import java.io.InputStream;
import org.libsdl.app.SDLActivity;

// Native Crispy Doom for Circle OS. Extracts the bundled freedoom IWAD to the
// app's files dir (crispy finds it via DOOMWADDIR, set in the manifest), then
// hands off to SDLActivity which loads libmain.so -> SDL_main == crispy main().
public class DoomActivity extends SDLActivity {

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        // DOOM is a landscape game; in portrait crispy's widescreen sizing runs
        // past the column buffer (R_DrawColumn range error). Force it here so we
        // don't depend on the manifest being honoured by the launcher/OEM skin.
        setRequestedOrientation(ActivityInfo.SCREEN_ORIENTATION_SENSOR_LANDSCAPE);
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

    @Override
    protected String[] getLibraries() {
        return new String[] { "SDL2", "SDL2_mixer", "SDL2_net", "main" };
    }
}
