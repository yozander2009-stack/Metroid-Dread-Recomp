// Included by the two platform importers after their filesystem helpers.
static bool randovania_import_info(const std::string& root, const std::string& romfs,
                                   std::string& patch, std::string* err) {
    std::string path;
    bool duplicate = false, older = false, needs_exlaunch = false;
    walk(root, "", [&](const std::string& relative, bool dir, long long) {
        if (dir) return;
        const auto separator = relative.find_last_of("\\/");
        std::string leaf = relative.substr(separator == std::string::npos ? 0 : separator + 1);
        for (char& c : leaf) if (c >= 'a' && c <= 'z') c -= 'a' - 'A';
        if (leaf == SWR_RANDOVANIA_BUILD_ID ".IPS" || leaf == "RANDOVANIA.IPS") {
            if (!path.empty()) duplicate = true;
            else path = join(root, relative);
        } else if (leaf == "49161D9CCBC15DF944D0B6278A3C446C006B0BE8.IPS") {
            older = true;
        } else if (leaf == "REPLACEMENTS.JSON" || leaf == "RDVHASH" || leaf == "SUBSDK9") {
            needs_exlaunch = true;
        }
    });
    auto fail = [&](const char* message) {
        if (err) *err = message;
        return false;
    };
    if (path.empty()) {
        if (older) return fail("Randovania needs the executable patch for Metroid Dread 2.1.0. Import the complete PKG export.");
        return true;
    }
    if (duplicate) return fail("Multiple Randovania executable patches found. Import one complete seed at a time.");
    if (needs_exlaunch) return fail("Export a single-player Randovania PKG with unique save slots disabled.");
    if (romfs.empty()) return fail("The Randovania export is missing its romfs folder.");
    if (file_size(path) == SWR_RANDOVANIA_IPS_SIZE) patch = read_text(path);
    if (!swr_randovania_validate_ips(reinterpret_cast<const unsigned char*>(patch.data()), patch.size()))
        return fail("The Randovania executable patch is incomplete or unsupported. Import a complete 2.1.0 PKG export.");
    return true;
}

static bool import_randovania(const std::string& romfs, const std::string& patch,
                              const std::string& target, std::string* err) {
    if (dir_exists(target) || file_exists(target)) {
        if (err) *err = "A mod with this name already exists. Remove it in Mods & Texture Packs before importing this Randovania seed.";
        return false;
    }
    // Stage away from mods so a failed copy cannot be loaded as a partial seed.
    std::string staging = join(P.data, "randovania-import-" + now_stamp());
    if (!create_randovania_stage(staging)) {
        if (err) *err = "Could not create the Randovania import folder.";
        return false;
    }
    bool ok = copy_tree(romfs, join(staging, "romfs"), err, true);
    if (ok && (!write_text(join(staging, SWR_RANDOVANIA_MARKER), patch)
               || !make_dirs(mods_dir()) || !publish_randovania_stage(staging, target))) {
        if (err) *err = "Could not finish installing the Randovania seed.";
        ok = false;
    }
    if (!ok) clean_randovania_stage(staging);
    return ok;
}
