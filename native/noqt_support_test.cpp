#include "noqt_filesystem.h"
#include "settings.h"
#include <fstream>
#include <iostream>
#include <stdexcept>

static void expect(bool value, const char* what) {
    if (!value) throw std::runtime_error(what);
}

int main(int argc, char** argv) {
    try {
        if (argc != 2) throw std::runtime_error("Expected a fresh test directory");
        const auto root = std::filesystem::absolute(std::filesystem::u8path(argv[1]));
        expect(!std::filesystem::exists(root), "Test directory already exists");
        std::filesystem::create_directories(root / "assets");
        std::ofstream(root / "assets" / "test.txt", std::ios::binary) << "resource";
        music::NativeFileSystem fs(root / "assets");
        auto asPath = [](const std::filesystem::path& p) {
            auto value = p.u8string();
            return muse::io::path_t(std::string(value.begin(), value.end()));
        };
        expect(bool(fs.readFile(":/test.txt").ret), "Resource read");
        expect(!fs.readFile(":/../outside.txt").ret, "Resource traversal rejected");
        expect(!fs.writeFile(":/test.txt", muse::ByteArray("bad", 3)), "Resource write rejected");
        expect(!fs.writeFile(asPath(root / "assets/test.txt"), muse::ByteArray("bad", 3)), "Absolute resource write rejected");
        const auto file = asPath(root / std::filesystem::u8path("测试.bin"));
        expect(bool(fs.writeFile(file, muse::ByteArray("abcdef", 6))), "Unicode write");
        auto data = fs.readFile(file);
        expect(bool(data.ret) && data.val == muse::ByteArray("abcdef", 6), "Read/write content");
        auto stream = fs.openStream(file, muse::io::OpenMode::WriteOnly);
        expect(bool(stream.ret), "Open stream");
        expect(bool(fs.writeToStream(stream.val, muse::ByteArray("abcd", 4))), "Write stream");
        expect(bool(fs.writeToStream(stream.val, muse::ByteArray("XY", 2), 1)), "Seek overwrite");
        expect(bool(fs.closeStream(stream.val)), "Close stream");
        expect(fs.readFile(file).val == muse::ByteArray("aXYd", 4), "Stream content");
        expect(!fs.closeStream(stream.val), "Stale stream rejected");
        const auto scan = fs.scanFiles(asPath(root), {"*.bin"}, muse::io::ScanMode::FilesInCurrentDir);
        expect(bool(scan.ret) && scan.val.size() == 1, "Scan filter");
        auto* settings = muse::Settings::instance();
        const muse::Settings::Key key("test", "value"), added("test", "added");
        settings->setDefaultValue(key, muse::Val(1));
        settings->beginTransaction();
        settings->setLocalValue(key, muse::Val(2));
        settings->setDefaultValue(added, muse::Val(3));
        settings->setDescription(added, "new default");
        settings->commitTransaction();
        expect(settings->value(key).toInt() == 2 && settings->defaultValue(added).toInt() == 3,
               "Commit preserves new default metadata");
        settings->beginTransaction();
        settings->setLocalValue(key, muse::Val(4));
        settings->rollbackTransaction();
        expect(settings->value(key).toInt() == 2, "Rollback");
        settings->reset();
        expect(settings->value(key).toInt() == 1, "Reset defaults");
        std::cout << "native filesystem/settings checks passed\n";
    } catch (const std::exception& e) { std::cerr << e.what() << '\n'; return 1; }
}
