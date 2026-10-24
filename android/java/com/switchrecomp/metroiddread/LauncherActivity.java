package com.switchrecomp.metroiddread;

import android.app.Activity;
import android.content.Intent;
import android.graphics.Color;
import android.os.Bundle;
import android.view.View;
import android.widget.*;
import java.io.*;
import java.util.*;

/** Settings live in the same private INI as the native runtime. */
public final class LauncherActivity extends Activity {
    private final Map<String, String> settings = new LinkedHashMap<>();
    private final Map<String, Spinner> choices = new LinkedHashMap<>();
    private final Map<String, String[]> values = new LinkedHashMap<>();
    private final Map<String, CheckBox> checks = new LinkedHashMap<>();
    private String original;
    private TextView status, driverStatus;
    private boolean importing;
    private static final int IMPORT_DRIVER=42;
    private SeekBar volume;
    private boolean vulkan;
    private final int foreground = Color.rgb(231, 239, 245);
    private final int muted = Color.rgb(158, 174, 188);

    @Override public void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);
        getWindow().addFlags(android.view.WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON);
        try {
            String capabilities = read(getAssets().open("switchrecomp/capabilities.txt"));
            vulkan = capabilities.contains("vulkan=1");
            File ini = new File(getFilesDir(), "MetroidDread.ini");
            loadSettings(read(ini.isFile() ? new FileInputStream(ini) : getAssets().open("switchrecomp/game.ini")));
            createScreen();
        } catch (IOException e) {
            TextView error = new TextView(this);
            error.setText("Could not read game settings: " + e.getMessage());
            setContentView(error);
        }
    }

    private void loadSettings(String text) {
        original = text;
        settings.clear(); choices.clear(); values.clear(); checks.clear();
        for (String line : original.split("\\r?\\n")) {
            String entry = line.trim();
            if (entry.startsWith(";") || entry.startsWith("#") || entry.startsWith("[")) continue;
            int equal = entry.indexOf('=');
            if (equal > 0) settings.put(entry.substring(0, equal).trim(), entry.substring(equal + 1).trim());
        }
    }
    @Override public void onResume() {
        super.onResume();
        File ini = new File(getFilesDir(), "MetroidDread.ini");
        try {
            if (original != null && ini.isFile()) {
                String current = read(new FileInputStream(ini));
                if (!current.equals(original)) { loadSettings(current); createScreen(); }
            }
        } catch (IOException e) { if (status != null) status.setText("Could not reload settings: " + e.getMessage()); }
        updateDriverStatus();
    }

    private int dp(int n) { return Math.round(n * getResources().getDisplayMetrics().density); }
    private String setting(String key, String fallback) { return settings.containsKey(key) ? settings.get(key) : fallback; }
    private TextView text(String title, int size, int color) {
        TextView view = new TextView(this);
        view.setText(title); view.setTextSize(size); view.setTextColor(color);
        view.setPadding(0, dp(4), 0, dp(4));
        return view;
    }
    private LinearLayout column() {
        LinearLayout layout = new LinearLayout(this);
        layout.setOrientation(LinearLayout.VERTICAL);
        layout.setPadding(dp(12), 0, dp(12), 0);
        return layout;
    }
    private void choice(LinearLayout parent, String key, String label, String[] labels, String[] options, String fallback) {
        parent.addView(text(label, 14, muted));
        Spinner spinner = new Spinner(this);
        ArrayAdapter<String> adapter = new ArrayAdapter<>(this, android.R.layout.simple_spinner_item, labels);
        adapter.setDropDownViewResource(android.R.layout.simple_spinner_dropdown_item);
        spinner.setAdapter(adapter);
        String selected = setting(key, fallback);
        for (int i = 0; i < options.length; i++) if (options[i].equals(selected)) spinner.setSelection(i);
        parent.addView(spinner, new LinearLayout.LayoutParams(-1, dp(48)));
        choices.put(key, spinner); values.put(key, options);
    }
    private void check(LinearLayout parent, String key, String label, String fallback) {
        CheckBox view = new CheckBox(this);
        view.setText(label); view.setTextColor(foreground);
        view.setChecked(!setting(key, fallback).equals("0"));
        parent.addView(view); checks.put(key, view);
    }

    private void select(String key, String value) {
        String[] options = values.get(key);
        for (int i = 0; i < options.length; i++) if (options[i].equals(value)) choices.get(key).setSelection(i);
    }
    private void preset(boolean performance) {
        select("resolution", performance ? "0.75" : "1");
        select("anisotropy", performance ? "0" : "4");
        checks.get("shadows").setChecked(!performance);
        checks.get("antialiasing").setChecked(!performance);
        status.setText(performance ? "Performance preset: 75% resolution, shadows and FXAA off. Save or Play to apply."
                : "Quality preset: native resolution, shadows, FXAA and 4× filtering. Save or Play to apply.");
    }

    private void createScreen() {
        LinearLayout root = column(); root.setPadding(dp(24), dp(18), dp(24), dp(12));
        root.setBackgroundColor(Color.rgb(12, 19, 27));
        root.addView(text("METROID DREAD", 26, Color.rgb(255, 69, 83)));
        root.addView(text("Android settings • Changes apply when you launch the game", 14, muted));
        LinearLayout actions = new LinearLayout(this);
        Button play = new Button(this); play.setText("PLAY");
        play.setOnClickListener(v -> save(true));
        Button save = new Button(this); save.setText("Save settings");
        save.setOnClickListener(v -> save(false));
        actions.addView(play, new LinearLayout.LayoutParams(0, dp(52), 1));
        actions.addView(save, new LinearLayout.LayoutParams(0, dp(52), 1));
        root.addView(actions);
        status = text("", 14, muted); root.addView(status);
        ScrollView scroll = new ScrollView(this);
        LinearLayout panels = new LinearLayout(this);
        LinearLayout graphics = column(), display = column();
        panels.addView(graphics, new LinearLayout.LayoutParams(0, -2, 1));
        panels.addView(display, new LinearLayout.LayoutParams(0, -2, 1));
        scroll.addView(panels); root.addView(scroll, new LinearLayout.LayoutParams(-1, 0, 1));
        graphics.addView(text("GRAPHICS", 18, foreground));
        choice(graphics, "renderer", "Renderer", vulkan
                ? new String[]{"OpenGL ES 3.2", "Vulkan (experimental)"} : new String[]{"OpenGL ES 3.2"},
                vulkan ? new String[]{"opengl", "vulkan_native"} : new String[]{"opengl"}, "opengl");
        choice(graphics, "resolution", "Internal resolution", new String[]{"50% • faster", "75%", "100% • native", "125%", "150%", "200%"},
                new String[]{"0.5", "0.75", "1", "1.25", "1.5", "2"}, "1");
        choice(graphics, "fps_limit", "FPS limit", new String[]{"60 FPS", "90 FPS", "120 FPS", "Unlimited • experimental"},
                new String[]{"1", "90", "120", "0"}, "1");
        check(graphics, "shadows", "Render shadows", "1");
        check(graphics, "antialiasing", "FXAA anti-aliasing", "0");
        choice(graphics, "anisotropy", "Anisotropic filtering", new String[]{"Game default", "2×", "4×", "8×", "16×"},
                new String[]{"0", "2", "4", "8", "16"}, "0");
        check(graphics, "wait_for_shaders", "Wait for shaders • avoids missing objects", "1");
        check(graphics, "forced_vsync", "VSync", "1");
        check(graphics, "show_fps", "Show FPS counter", "1");
        graphics.addView(text("The FPS limit is a target. Higher limits do not guarantee faster rendering or correct game speed.", 13, muted));
        display.addView(text("DISPLAY & CONTROLS", 18, foreground));
        choice(display, "aspect", "Aspect ratio", new String[]{"4:3 • Nova screen", "16:9 • original", "16:10", "21:9"},
                new String[]{"4:3", "16:9", "16:10", "21:9"}, "4:3");
        check(display, "touch_controls", "Enable touch controls • OpenGL ES", "0");
        check(display, "auto_hide_touch_controls", "Hide touch controls when a controller is connected", "1");
        check(display, "skip_cutscenes", "Allow holding Minus to skip cutscenes", "0");
        check(display, "cutscenes_60fps", "60 FPS cutscenes • experimental", "0");
        display.addView(text("Volume", 14, muted));
        volume = new SeekBar(this); volume.setMax(100);
        try { volume.setProgress(Integer.parseInt(setting("volume", "100"))); }
        catch (NumberFormatException e) { volume.setProgress(100); }
        display.addView(volume);
        display.addView(text("GRAPHICS DRIVER", 18, foreground));
        if (vulkan) {
            List<DriverPackage.Info> drivers=DriverPackage.list(this);
            List<String> labels=new ArrayList<>(), ids=new ArrayList<>();
            labels.add("System driver"); ids.add("");
            for (DriverPackage.Info driver:drivers) { labels.add(driver.name); ids.add(driver.id); }
            String selected=setting("custom_driver", "");
            if (!ids.contains(selected)) { labels.add("Unavailable driver • import again or choose System"); ids.add(selected); }
            choice(display,"custom_driver","Vulkan driver",labels.toArray(new String[0]),ids.toArray(new String[0]),"");
            Button install=new Button(this); install.setText("Import custom driver ZIP");
            install.setOnClickListener(v -> {
                if (importing) return;
                Intent picker=new Intent(Intent.ACTION_OPEN_DOCUMENT);
                picker.addCategory(Intent.CATEGORY_OPENABLE); picker.setType("*/*");
                startActivityForResult(picker,IMPORT_DRIVER);
            });
            display.addView(install);
            Button remove=new Button(this); remove.setText("Remove selected custom driver");
            remove.setOnClickListener(v -> removeDriver()); display.addView(remove);
            display.addView(text("Compatible ARM64 Vulkan driver ZIPs with meta.json. Applies on next launch; OpenGL ES always uses the system driver.",13,muted));
            driverStatus=text("",13,muted); display.addView(driverStatus); updateDriverStatus();
        } else display.addView(text("Custom drivers require a Vulkan-enabled build.",13,muted));
        display.addView(text("Your game data and saves stay in this app when updating. The game pauses in the background. Android Back exits to this screen.", 13, muted));
        LinearLayout presets = new LinearLayout(this);
        Button fast = new Button(this); fast.setText("Performance preset"); fast.setOnClickListener(v -> preset(true));
        Button quality = new Button(this); quality.setText("Quality preset"); quality.setOnClickListener(v -> preset(false));
        presets.addView(fast, new LinearLayout.LayoutParams(0, dp(52), 1));
        presets.addView(quality, new LinearLayout.LayoutParams(0, dp(52), 1));
        graphics.addView(presets);
        setContentView(root);
        File romfs = new File(getFilesDir(), "MetroidDread_data/romfs");
        if (!romfs.isDirectory()) status.setText("Game data missing. Deploy your RomFS from the build computer first.");
    }

    private Map<String,String> controls() {
        Map<String, String> updates = new LinkedHashMap<>();
        for (String key : choices.keySet()) updates.put(key, values.get(key)[choices.get(key).getSelectedItemPosition()]);
        for (String key : checks.keySet()) updates.put(key, checks.get(key).isChecked() ? "1" : "0");
        updates.put("volume", Integer.toString(volume.getProgress()));
        updates.put("docked", updates.get("aspect").equals("16:9") ? "0" : "1");
        return updates;
    }
    private void save(boolean launch) {
        if (importing) { status.setText("Wait for the driver import to finish"); return; }
        Map<String,String> updates=controls();
        StringBuilder output = new StringBuilder();
        Set<String> written = new HashSet<>();
        for (String line : original.split("\\r?\\n")) {
            String trimmed = line.trim(); int equal = trimmed.indexOf('=');
            String key = equal > 0 ? trimmed.substring(0, equal).trim() : "";
            if (!trimmed.startsWith(";") && !trimmed.startsWith("#") && updates.containsKey(key)) {
                output.append(key).append('=').append(updates.get(key)).append('\n'); written.add(key);
            } else output.append(line).append('\n');
        }
        for (String key : updates.keySet()) if (!written.contains(key)) output.append(key).append('=').append(updates.get(key)).append('\n');
        File target = new File(getFilesDir(), "MetroidDread.ini"), temporary = new File(getFilesDir(), "MetroidDread.ini.tmp");
        try {
            try (FileOutputStream stream = new FileOutputStream(temporary)) {
                stream.write(output.toString().getBytes("UTF-8")); stream.getFD().sync();
            }
            if (!temporary.renameTo(target)) throw new IOException("Could not replace settings file");
            original = output.toString(); settings.putAll(updates); status.setText("Settings saved");
            if (launch) {
                String driver="vulkan_native".equals(updates.get("renderer")) ? updates.get("custom_driver") : "";
                DriverPackage.get(this,driver); // Validate the selected package before starting the game process.
                startActivity(new Intent(this, GameActivity.class).putExtra("custom_driver",driver));
            }
        } catch (Exception e) { status.setText("Could not launch/save settings: " + e.getMessage()); }
    }

    private void updateDriverStatus() {
        if (driverStatus==null) return;
        File file=new File(getFilesDir(),"driver-status.txt");
        try { driverStatus.setText(file.isFile() && file.length()<8192 ? "Last launch: " + read(new FileInputStream(file)).trim() : ""); }
        catch (IOException e) { driverStatus.setText("Could not read driver status"); }
    }
    @Override protected void onActivityResult(int request,int result,Intent data) {
        super.onActivityResult(request,result,data);
        if (request!=IMPORT_DRIVER || result!=RESULT_OK || data==null || data.getData()==null) return;
        importing=true; status.setText("Importing driver…");
        android.net.Uri uri=data.getData();
        new Thread(() -> {
            try {
                DriverPackage.Info driver=DriverPackage.install(this,uri);
                runOnUiThread(() -> {
                    importing=false; settings.putAll(controls()); settings.put("custom_driver",driver.id);
                    createScreen(); status.setText("Imported " + driver.name + ". Save settings or Play to use it.");
                });
            } catch (Exception e) { runOnUiThread(() -> { importing=false; status.setText("Import failed: " + e.getMessage()); }); }
        },"DriverImport").start();
    }
    private void removeDriver() {
        if (importing) return;
        Map<String,String> current=controls(); String id=current.get("custom_driver");
        if (id==null || id.isEmpty()) { status.setText("System driver cannot be removed"); return; }
        try {
            DriverPackage.remove(this,id); settings.putAll(current); settings.put("custom_driver","");
            createScreen(); save(false); status.setText("Custom driver removed; System driver selected");
        } catch (Exception e) { status.setText("Could not remove driver: " + e.getMessage()); }
    }

    private static String read(InputStream input) throws IOException {
        try (InputStream stream = input; ByteArrayOutputStream out = new ByteArrayOutputStream()) {
            byte[] buffer = new byte[8192]; int n;
            while ((n = stream.read(buffer)) != -1) out.write(buffer, 0, n);
            return out.toString("UTF-8");
        }
    }
}
