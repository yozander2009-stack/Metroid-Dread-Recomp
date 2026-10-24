package com.switchrecomp.metroiddread;

import android.content.Context;
import android.net.Uri;
import android.os.Build;
import org.json.JSONObject;
import java.io.*;
import java.nio.file.*;
import java.security.MessageDigest;
import java.util.*;
import java.util.zip.*;

/** Standard AdrenoTools meta.json ZIP packages, independent of driver vendor. */
final class DriverPackage {
    private static final long MAX_ZIP = 128L << 20, MAX_TOTAL = 256L << 20, MAX_FILE = 128L << 20;
    static final class Info {
        final String id, name, description, library;
        final File directory, installed;
        Info(String id, String name, String description, File installed, File binary) {
            this.id=id; this.name=name; this.description=description; this.installed=installed;
            directory=binary.getParentFile(); library=binary.getName();
        }
    }
    static File root(Context context) { return new File(context.getFilesDir(), "drivers"); }
    static Info get(Context context, String id) throws Exception {
        if (id == null || id.isEmpty()) return null;
        if (!id.matches("[a-f0-9]{64}")) throw new IOException("Invalid driver selection");
        File installed = new File(root(context), id);
        JSONObject descriptor = json(new File(installed, "installed.json"));
        File binary = inside(installed, descriptor.getString("library"));
        elf(binary);
        return new Info(id, descriptor.getString("name"), descriptor.optString("description"), installed, binary);
    }
    static List<Info> list(Context context) {
        List<Info> drivers = new ArrayList<>(); File[] folders = root(context).listFiles();
        if (folders != null) for (File folder : folders) {
            if (!folder.getName().matches("[a-f0-9]{64}")) continue;
            try { drivers.add(get(context, folder.getName())); } catch (Exception ignored) { /* Not a valid installed package. */ }
        }
        drivers.sort(Comparator.comparing(info -> info.name)); return drivers;
    }
    static Info install(Context context, Uri uri) throws Exception {
        File archive = File.createTempFile("driver-", ".zip", context.getCacheDir());
        File staging = new File(root(context), ".import-" + UUID.randomUUID());
        try {
            MessageDigest digest = MessageDigest.getInstance("SHA-256");
            try (InputStream in = context.getContentResolver().openInputStream(uri);
                 OutputStream out = new FileOutputStream(archive)) {
                if (in == null) throw new IOException("Cannot open ZIP");
                byte[] bytes = new byte[65536]; int n; long count=0;
                while ((n=in.read(bytes)) != -1) {
                    count+=n; if (count>MAX_ZIP) throw new IOException("Driver ZIP exceeds 128 MiB");
                    digest.update(bytes,0,n); out.write(bytes,0,n);
                }
            }
            StringBuilder hash = new StringBuilder(); for (byte b : digest.digest()) hash.append(String.format(Locale.ROOT,"%02x", b & 255));
            String id = hash.toString();
            File destination = new File(root(context), id);
            if (destination.isDirectory()) {
                try { return get(context,id); } catch (Exception invalid) { erase(destination); }
            }
            if (!staging.mkdirs()) throw new IOException("Cannot create driver import directory");
            Set<String> paths = new HashSet<>(); List<File> metadata = new ArrayList<>();
            long total=0; int entries=0;
            try (ZipInputStream zip = new ZipInputStream(new FileInputStream(archive))) {
                ZipEntry entry; byte[] bytes = new byte[65536];
                while ((entry=zip.getNextEntry()) != null) {
                    if (++entries>256) throw new IOException("Too many files in driver ZIP");
                    if (entry.isDirectory() && (entry.getName().equals("./") || entry.getName().equals("."))) continue;
                    File target=inside(staging,entry.getName());
                    if (!paths.add(target.getCanonicalPath())) throw new IOException("Duplicate ZIP path");
                    if (entry.isDirectory()) {
                        if (!target.isDirectory() && !target.mkdirs()) throw new IOException("Cannot create driver directory");
                        continue;
                    }
                    File parent=target.getParentFile();
                    if (!parent.isDirectory() && !parent.mkdirs()) throw new IOException("Cannot create driver directory");
                    long size=0; int n;
                    try (OutputStream out = new FileOutputStream(target)) {
                        while ((n=zip.read(bytes)) != -1) {
                            size+=n; total+=n;
                            if (size>MAX_FILE || total>MAX_TOTAL) throw new IOException("Unpacked driver ZIP is too large");
                            out.write(bytes,0,n);
                        }
                    }
                    if (target.getName().equals("meta.json")) metadata.add(target);
                }
            }
            if (metadata.size()!=1) throw new IOException("Driver ZIP must contain exactly one meta.json");
            File meta=metadata.get(0); JSONObject info=json(meta);
            if (info.optInt("schemaVersion",1)!=1) throw new IOException("Unsupported driver package schema");
            if (info.optInt("minApi",28)>Build.VERSION.SDK_INT) throw new IOException("Driver requires a newer Android version");
            String library=info.getString("libraryName");
            if (!library.matches("[A-Za-z0-9_.+-]+\\.so")) throw new IOException("Invalid driver libraryName");
            File binary=inside(meta.getParentFile(),library); elf(binary);
            String name=info.getString("name").replaceAll("[\\p{Cntrl}]"," ").trim();
            if (name.isEmpty() || name.length()>200) throw new IOException("Invalid driver name");
            String relative=staging.getCanonicalFile().toPath().relativize(binary.toPath()).toString();
            JSONObject descriptor=new JSONObject().put("name",name).put("library",relative)
                    .put("description", info.optString("description") + "\n" + info.optString("driverVersion"));
            Files.write(new File(staging,"installed.json").toPath(),descriptor.toString().getBytes("UTF-8"));
            if (!staging.renameTo(destination)) throw new IOException("Cannot finish driver installation");
            return get(context,id);
        } finally { archive.delete(); erase(staging); }
    }
    private static JSONObject json(File file) throws Exception {
        if (!file.isFile() || file.length()>65536) throw new IOException("Driver metadata missing or too large");
        return new JSONObject(new String(Files.readAllBytes(file.toPath()),"UTF-8"));
    }
    private static File inside(File directory,String name) throws IOException {
        if (name.isEmpty() || name.startsWith("/") || name.contains("\\")) throw new IOException("Invalid path in driver ZIP");
        File target=new File(directory,name).getCanonicalFile();
        if (!target.getPath().startsWith(directory.getCanonicalPath()+File.separator)) throw new IOException("Driver ZIP path escapes its directory");
        return target;
    }
    private static void elf(File library) throws IOException {
        byte[] header=new byte[64];
        try (DataInputStream in=new DataInputStream(new FileInputStream(library))) { in.readFully(header); }
        if (header[0]!=127 || header[1]!='E' || header[2]!='L' || header[3]!='F' || header[4]!=2 || header[5]!=1
                || header[16]!=3 || header[17]!=0 || (header[18]&255)!=183 || header[19]!=0)
            throw new IOException("Driver library must be an ARM64 ELF shared object");
    }
    static void remove(Context context, String id) throws Exception {
        Info info=get(context,id); if (info!=null) erase(info.installed);
    }
    private static void erase(File file) throws IOException {
        if (!Files.exists(file.toPath(),LinkOption.NOFOLLOW_LINKS)) return;
        if (file.isDirectory() && !Files.isSymbolicLink(file.toPath())) {
            File[] children=file.listFiles(); if (children==null) throw new IOException("Cannot read driver directory");
            for (File child:children) erase(child);
        }
        Files.delete(file.toPath());
    }
}
