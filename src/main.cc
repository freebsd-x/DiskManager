// DiskManager.xapp — Disk & Partition Manager for FreeBSD-X
//
// Layout (880×560):
//   Title bar  │ main row: left panel (250px) | right panel (flex)
//   Left  : Disks list (scrollable) + ZFS Pools section
//   Right : Partitions for the selected disk (scrollable)
//   Footer: status label + Mount / Unmount / Format / Eject buttons
//           (Format mode shows inline fs-type / label inputs)

#include "xkit/widget.h"
#include "xkit/application.h"
#include "xkit/xbus_client.h"
#include "shared/protocol.h"
#include <memory>
#include <string>
#include <vector>
#include <mutex>
#include <thread>
#include <chrono>
#include <cstring>
#include <cstdio>
#include <cstdlib>
#include <functional>

using namespace fbx::xkit;
using namespace fbx::shared;

// ── Format helpers ────────────────────────────────────────────────────────────

static std::string fmtBytes(uint64_t b) {
    if (b == 0) return "0 B";
    const char* units[] = { "B", "KB", "MB", "GB", "TB" };
    int u = 0;
    double v = static_cast<double>(b);
    while (v >= 1024.0 && u < 4) { v /= 1024.0; ++u; }
    char buf[32];
    if (u == 0) std::snprintf(buf, sizeof(buf), "%.0f %s", v, units[u]);
    else        std::snprintf(buf, sizeof(buf), "%.1f %s", v, units[u]);
    return buf;
}


// ── App ───────────────────────────────────────────────────────────────────────

int main(int argc, char** argv) {
    (void)argc; (void)argv;

    // ── Shared state ──────────────────────────────────────────────────────────
    auto selected_disk      = std::make_shared<std::string>("");
    auto selected_partition = std::make_shared<std::string>("");  // full device name e.g. "ada0p1"
    auto selected_mounted   = std::make_shared<bool>(false);
    auto selected_removable = std::make_shared<bool>(false);
    auto status_str         = std::make_shared<std::string>("");
    auto format_fs          = std::make_shared<std::string>("ufs");  // "ufs" or "fat32"

    // Widget pointers populated inside Run
    auto disk_col_ptr     = std::make_shared<std::shared_ptr<FlexColumn>>();
    auto part_col_ptr     = std::make_shared<std::shared_ptr<FlexColumn>>();
    auto pool_col_ptr     = std::make_shared<std::shared_ptr<FlexColumn>>();
    auto section_hdr_ptr  = std::make_shared<std::shared_ptr<TextWidget>>();
    auto status_lbl_ptr   = std::make_shared<std::shared_ptr<TextWidget>>();
    auto footer_row_ptr   = std::make_shared<std::shared_ptr<FlexRow>>();
    auto mount_input_ptr  = std::make_shared<std::shared_ptr<TextInputWidget>>();
    auto label_input_ptr  = std::make_shared<std::shared_ptr<TextInputWidget>>();
    auto fs_ufs_btn_ptr   = std::make_shared<std::shared_ptr<ButtonWidget>>();
    auto fs_fat_btn_ptr   = std::make_shared<std::shared_ptr<ButtonWidget>>();

    // Deferred lambdas
    auto refresh_all         = std::make_shared<std::function<void()>>();
    auto refresh_partitions  = std::make_shared<std::function<void()>>();
    auto show_normal_footer  = std::make_shared<std::function<void()>>();
    auto show_mount_footer   = std::make_shared<std::function<void()>>();
    auto show_format_footer  = std::make_shared<std::function<void()>>();

    static XBusClient* g_xbus = nullptr;

    auto set_status = [status_lbl_ptr, status_str](const std::string& msg) {
        *status_str = msg;
        if (*status_lbl_ptr) (*status_lbl_ptr)->setText(msg);
    };

    // ── Build root ────────────────────────────────────────────────────────────
    auto root_col = std::make_shared<FlexColumn>(std::vector<std::shared_ptr<Widget>>{});
    root_col->setBackgroundColor(Color{0.10f, 0.10f, 0.10f, 1.0f});

    return XApplication::Run(root_col, "/tmp/freebsdx.sock", /*sandboxed=*/false,
                             /*is_shell=*/false,
        [&](XBusClient& xbus) {
            g_xbus = &xbus;
            XApplication::setTitle("Disk Manager");

            // ── Title row ────────────────────────────────────────────────────
            auto title_lbl = std::make_shared<TextWidget>("Disk Manager", 16.0f);
            title_lbl->setColor(Color{0.95f, 0.95f, 0.95f, 1.0f});
            title_lbl->setFlexGrow(1.0f);

            auto refresh_btn = std::make_shared<ButtonWidget>("↻ Refresh",
                [refresh_all]() {
                    if (*refresh_all) (*refresh_all)();
                });
            refresh_btn->setFontSize(12.0f);
            refresh_btn->setFixedSize(80.0f, 28.0f);
            refresh_btn->setBackgroundColor(Color{0.22f, 0.22f, 0.25f, 1.0f});

            auto title_row = std::make_shared<FlexRow>(
                std::vector<std::shared_ptr<Widget>>{ title_lbl, refresh_btn });
            title_row->setCrossAxisAlignment(CrossAxisAlignment::Center);
            title_row->setFixedSize(0.0f, 44.0f);
            title_row->setPadding(10.0f);
            title_row->setGap(8.0f);
            title_row->setBackgroundColor(Color{0.12f, 0.12f, 0.14f, 1.0f});

            // ── Left panel ───────────────────────────────────────────────────
            auto disks_hdr = std::make_shared<TextWidget>("DISKS", 11.0f);
            disks_hdr->setColor(Color{0.50f, 0.55f, 0.65f, 1.0f});
            disks_hdr->setPadding(6.0f);

            auto disks_hdr_row = std::make_shared<FlexRow>(
                std::vector<std::shared_ptr<Widget>>{ disks_hdr });
            disks_hdr_row->setFixedSize(0.0f, 28.0f);
            disks_hdr_row->setCrossAxisAlignment(CrossAxisAlignment::Center);
            disks_hdr_row->setBackgroundColor(Color{0.14f, 0.14f, 0.16f, 1.0f});

            auto disk_col = std::make_shared<FlexColumn>(std::vector<std::shared_ptr<Widget>>{});
            disk_col->setGap(1.0f);
            *disk_col_ptr = disk_col;

            auto disk_scroll = std::make_shared<ScrollViewWidget>(disk_col);
            disk_scroll->setFlexGrow(1.0f);

            // ZFS pools section
            auto pools_hdr = std::make_shared<TextWidget>("ZFS POOLS", 11.0f);
            pools_hdr->setColor(Color{0.50f, 0.65f, 0.55f, 1.0f});
            pools_hdr->setPadding(6.0f);

            auto pools_hdr_row = std::make_shared<FlexRow>(
                std::vector<std::shared_ptr<Widget>>{ pools_hdr });
            pools_hdr_row->setFixedSize(0.0f, 28.0f);
            pools_hdr_row->setCrossAxisAlignment(CrossAxisAlignment::Center);
            pools_hdr_row->setBackgroundColor(Color{0.14f, 0.14f, 0.16f, 1.0f});

            auto pool_col = std::make_shared<FlexColumn>(std::vector<std::shared_ptr<Widget>>{});
            pool_col->setGap(1.0f);
            *pool_col_ptr = pool_col;

            auto pool_scroll = std::make_shared<ScrollViewWidget>(pool_col);
            pool_scroll->setFixedSize(0.0f, 130.0f);

            auto left_panel = std::make_shared<FlexColumn>(
                std::vector<std::shared_ptr<Widget>>{
                    disks_hdr_row, disk_scroll,
                    std::make_shared<DividerWidget>(0.5f),
                    pools_hdr_row, pool_scroll });
            left_panel->setFixedSize(250.0f, 0.0f);
            left_panel->setBackgroundColor(Color{0.12f, 0.12f, 0.14f, 1.0f});

            // ── Right panel ──────────────────────────────────────────────────
            auto section_hdr = std::make_shared<TextWidget>("Select a disk", 13.0f);
            section_hdr->setColor(Color{0.70f, 0.75f, 0.90f, 1.0f});
            section_hdr->setPadding(8.0f);
            section_hdr->setFlexGrow(1.0f);
            *section_hdr_ptr = section_hdr;

            auto section_hdr_row = std::make_shared<FlexRow>(
                std::vector<std::shared_ptr<Widget>>{ section_hdr });
            section_hdr_row->setFixedSize(0.0f, 36.0f);
            section_hdr_row->setCrossAxisAlignment(CrossAxisAlignment::Center);
            section_hdr_row->setBackgroundColor(Color{0.14f, 0.14f, 0.17f, 1.0f});

            // Partitions render through a TableWidget (its own header); the
            // partition list column just hosts that table.
            auto part_col = std::make_shared<FlexColumn>(std::vector<std::shared_ptr<Widget>>{});
            part_col->setGap(1.0f);
            *part_col_ptr = part_col;

            auto part_scroll = std::make_shared<ScrollViewWidget>(part_col);
            part_scroll->setFlexGrow(1.0f);

            auto right_panel = std::make_shared<FlexColumn>(
                std::vector<std::shared_ptr<Widget>>{
                    section_hdr_row, part_scroll });
            right_panel->setFlexGrow(1.0f);
            right_panel->setBackgroundColor(Color{0.10f, 0.10f, 0.12f, 1.0f});

            // ── Main content row ─────────────────────────────────────────────
            auto main_row = std::make_shared<FlexRow>(
                std::vector<std::shared_ptr<Widget>>{
                    left_panel,
                    std::make_shared<DividerWidget>(1.0f),
                    right_panel });
            main_row->setFlexGrow(1.0f);

            // ── Footer: normal mode ──────────────────────────────────────────
            auto status_lbl = std::make_shared<TextWidget>("Ready", 12.0f);
            status_lbl->setColor(Color{0.60f, 0.60f, 0.60f, 1.0f});
            status_lbl->setFlexGrow(1.0f);
            *status_lbl_ptr = status_lbl;

            auto mount_btn = std::make_shared<ButtonWidget>("Mount",
                [show_mount_footer]() {
                    if (*show_mount_footer) (*show_mount_footer)();
                });
            mount_btn->setFontSize(12.0f);
            mount_btn->setFixedSize(72.0f, 28.0f);
            mount_btn->setBackgroundColor(Color{0.20f, 0.35f, 0.22f, 1.0f});

            auto unmount_btn = std::make_shared<ButtonWidget>("Unmount",
                [selected_partition, selected_mounted, set_status, show_normal_footer,
                 refresh_partitions]() {
                    if (selected_partition->empty()) {
                        set_status("Select a partition to unmount.");
                        return;
                    }
                    if (!*selected_mounted) {
                        set_status("Partition is not mounted.");
                        return;
                    }
                    if (!g_xbus) return;
                    DiskNameRequest req{};
                    std::strncpy(req.name_, selected_partition->c_str(), sizeof(req.name_) - 1);
                    g_xbus->Call("com.freebsdx.Disk",
                        static_cast<uint32_t>(DiskOpcode::Unmount),
                        {reinterpret_cast<const uint8_t*>(&req), sizeof(req)},
                        [set_status, refresh_partitions](const XBusMessage& r) {
                            DiskStatusResponse resp{};
                            std::memcpy(&resp, r.payload_, sizeof(resp));
                            resp.message_[sizeof(resp.message_)-1] = '\0';
                            set_status(resp.success_ ? "Unmounted." : std::string("Error: ") + resp.message_);
                            if (*refresh_partitions) (*refresh_partitions)();
                        });
                });
            unmount_btn->setFontSize(12.0f);
            unmount_btn->setFixedSize(80.0f, 28.0f);
            unmount_btn->setBackgroundColor(Color{0.30f, 0.20f, 0.10f, 1.0f});

            auto format_btn = std::make_shared<ButtonWidget>("Format…",
                [show_format_footer]() {
                    if (*show_format_footer) (*show_format_footer)();
                });
            format_btn->setFontSize(12.0f);
            format_btn->setFixedSize(80.0f, 28.0f);
            format_btn->setBackgroundColor(Color{0.35f, 0.18f, 0.18f, 1.0f});

            auto eject_btn = std::make_shared<ButtonWidget>("Eject",
                [selected_disk, selected_removable, set_status, show_normal_footer,
                 refresh_all]() {
                    if (selected_disk->empty()) {
                        set_status("Select a disk to eject.");
                        return;
                    }
                    if (!*selected_removable) {
                        set_status("Only removable disks can be ejected.");
                        return;
                    }
                    if (!g_xbus) return;
                    DiskNameRequest req{};
                    std::strncpy(req.name_, selected_disk->c_str(), sizeof(req.name_) - 1);
                    g_xbus->Call("com.freebsdx.Disk",
                        static_cast<uint32_t>(DiskOpcode::Eject),
                        {reinterpret_cast<const uint8_t*>(&req), sizeof(req)},
                        [set_status, refresh_all](const XBusMessage& r) {
                            DiskStatusResponse resp{};
                            std::memcpy(&resp, r.payload_, sizeof(resp));
                            resp.message_[sizeof(resp.message_)-1] = '\0';
                            set_status(resp.success_ ? "Ejected." : std::string("Error: ") + resp.message_);
                            if (*refresh_all) (*refresh_all)();
                        });
                });
            eject_btn->setFontSize(12.0f);
            eject_btn->setFixedSize(72.0f, 28.0f);
            eject_btn->setBackgroundColor(Color{0.22f, 0.22f, 0.25f, 1.0f});

            auto footer_row = std::make_shared<FlexRow>(
                std::vector<std::shared_ptr<Widget>>{
                    status_lbl, mount_btn, unmount_btn, format_btn, eject_btn });
            footer_row->setCrossAxisAlignment(CrossAxisAlignment::Center);
            footer_row->setFixedSize(0.0f, 44.0f);
            footer_row->setPadding(8.0f);
            footer_row->setGap(8.0f);
            footer_row->setBackgroundColor(Color{0.12f, 0.12f, 0.14f, 1.0f});
            *footer_row_ptr = footer_row;

            // ── Mount footer ─────────────────────────────────────────────────
            auto mount_input = std::make_shared<TextInputWidget>("/mnt");
            mount_input->setPlaceholder("Mount point, e.g. /mnt/usb");
            mount_input->setFlexGrow(1.0f);
            mount_input->setFixedSize(0.0f, 28.0f);
            *mount_input_ptr = mount_input;

            auto mount_confirm_btn = std::make_shared<ButtonWidget>("Mount",
                [selected_partition, mount_input_ptr, set_status,
                 show_normal_footer, refresh_partitions]() {
                    if (selected_partition->empty()) {
                        set_status("Select a partition to mount.");
                        if (*show_normal_footer) (*show_normal_footer)();
                        return;
                    }
                    if (!g_xbus) return;
                    std::string mpt = (*mount_input_ptr) ? (*mount_input_ptr)->getText() : "/mnt";
                    DiskMountRequest req{};
                    std::strncpy(req.device_,      selected_partition->c_str(), sizeof(req.device_)      - 1);
                    std::strncpy(req.mount_point_,  mpt.c_str(),                sizeof(req.mount_point_) - 1);
                    g_xbus->Call("com.freebsdx.Disk",
                        static_cast<uint32_t>(DiskOpcode::Mount),
                        {reinterpret_cast<const uint8_t*>(&req), sizeof(req)},
                        [set_status, refresh_partitions](const XBusMessage& r) {
                            DiskStatusResponse resp{};
                            std::memcpy(&resp, r.payload_, sizeof(resp));
                            resp.message_[sizeof(resp.message_)-1] = '\0';
                            set_status(resp.success_ ? "Mounted." : std::string("Error: ") + resp.message_);
                            if (*refresh_partitions) (*refresh_partitions)();
                        });
                    if (*show_normal_footer) (*show_normal_footer)();
                });
            mount_confirm_btn->setFontSize(12.0f);
            mount_confirm_btn->setFixedSize(72.0f, 28.0f);
            mount_confirm_btn->setBackgroundColor(Color{0.20f, 0.40f, 0.22f, 1.0f});

            auto mount_cancel_btn = std::make_shared<ButtonWidget>("Cancel",
                [show_normal_footer]() {
                    if (*show_normal_footer) (*show_normal_footer)();
                });
            mount_cancel_btn->setFontSize(12.0f);
            mount_cancel_btn->setFixedSize(72.0f, 28.0f);
            mount_cancel_btn->setBackgroundColor(Color{0.25f, 0.25f, 0.28f, 1.0f});

            auto mount_lbl = std::make_shared<TextWidget>("Mount partition at:", 12.0f);
            mount_lbl->setColor(Color{0.70f, 0.70f, 0.70f, 1.0f});

            // ── Format footer ─────────────────────────────────────────────────
            auto label_input = std::make_shared<TextInputWidget>("");
            label_input->setPlaceholder("Label (optional)");
            label_input->setFlexGrow(1.0f);
            label_input->setFixedSize(0.0f, 28.0f);
            *label_input_ptr = label_input;

            auto fs_ufs_btn = std::make_shared<ButtonWidget>("UFS",
                [format_fs, fs_ufs_btn_ptr, fs_fat_btn_ptr]() {
                    *format_fs = "ufs";
                    if (*fs_ufs_btn_ptr) (*fs_ufs_btn_ptr)->setBackgroundColor(Color{0.25f, 0.40f, 0.60f, 1.0f});
                    if (*fs_fat_btn_ptr) (*fs_fat_btn_ptr)->setBackgroundColor(Color{0.22f, 0.22f, 0.25f, 1.0f});
                });
            fs_ufs_btn->setFontSize(12.0f);
            fs_ufs_btn->setFixedSize(50.0f, 28.0f);
            fs_ufs_btn->setBackgroundColor(Color{0.25f, 0.40f, 0.60f, 1.0f}); // selected by default
            *fs_ufs_btn_ptr = fs_ufs_btn;

            auto fs_fat_btn = std::make_shared<ButtonWidget>("FAT32",
                [format_fs, fs_ufs_btn_ptr, fs_fat_btn_ptr]() {
                    *format_fs = "fat32";
                    if (*fs_fat_btn_ptr) (*fs_fat_btn_ptr)->setBackgroundColor(Color{0.25f, 0.40f, 0.60f, 1.0f});
                    if (*fs_ufs_btn_ptr) (*fs_ufs_btn_ptr)->setBackgroundColor(Color{0.22f, 0.22f, 0.25f, 1.0f});
                });
            fs_fat_btn->setFontSize(12.0f);
            fs_fat_btn->setFixedSize(58.0f, 28.0f);
            fs_fat_btn->setBackgroundColor(Color{0.22f, 0.22f, 0.25f, 1.0f});
            *fs_fat_btn_ptr = fs_fat_btn;

            auto format_confirm_btn = std::make_shared<ButtonWidget>("Format!",
                [selected_partition, format_fs, label_input_ptr, set_status,
                 show_normal_footer, refresh_partitions]() {
                    if (selected_partition->empty()) {
                        set_status("Select a partition to format.");
                        if (*show_normal_footer) (*show_normal_footer)();
                        return;
                    }
                    if (!g_xbus) return;
                    std::string lbl = (*label_input_ptr) ? (*label_input_ptr)->getText() : "";
                    DiskFormatRequest req{};
                    std::strncpy(req.device_,  selected_partition->c_str(), sizeof(req.device_)  - 1);
                    std::strncpy(req.fs_type_, format_fs->c_str(),          sizeof(req.fs_type_) - 1);
                    std::strncpy(req.label_,   lbl.c_str(),                 sizeof(req.label_)   - 1);
                    g_xbus->Call("com.freebsdx.Disk",
                        static_cast<uint32_t>(DiskOpcode::Format),
                        {reinterpret_cast<const uint8_t*>(&req), sizeof(req)},
                        [set_status, refresh_partitions](const XBusMessage& r) {
                            DiskStatusResponse resp{};
                            std::memcpy(&resp, r.payload_, sizeof(resp));
                            resp.message_[sizeof(resp.message_)-1] = '\0';
                            set_status(resp.success_ ? "Format complete." : std::string("Error: ") + resp.message_);
                            if (*refresh_partitions) (*refresh_partitions)();
                        });
                    if (*show_normal_footer) (*show_normal_footer)();
                });
            format_confirm_btn->setFontSize(12.0f);
            format_confirm_btn->setFixedSize(72.0f, 28.0f);
            format_confirm_btn->setBackgroundColor(Color{0.55f, 0.18f, 0.18f, 1.0f});

            auto format_cancel_btn = std::make_shared<ButtonWidget>("Cancel",
                [show_normal_footer]() {
                    if (*show_normal_footer) (*show_normal_footer)();
                });
            format_cancel_btn->setFontSize(12.0f);
            format_cancel_btn->setFixedSize(72.0f, 28.0f);
            format_cancel_btn->setBackgroundColor(Color{0.25f, 0.25f, 0.28f, 1.0f});

            auto format_lbl = std::make_shared<TextWidget>("Format:", 12.0f);
            format_lbl->setColor(Color{0.85f, 0.55f, 0.55f, 1.0f});

            // ── Footer mode switchers ─────────────────────────────────────────
            *show_normal_footer = [footer_row_ptr,
                                   status_lbl, mount_btn, unmount_btn,
                                   format_btn, eject_btn]() {
                auto fr = *footer_row_ptr;
                if (!fr) return;
                fr->clearChildren();
                fr->addChild(status_lbl);
                fr->addChild(mount_btn);
                fr->addChild(unmount_btn);
                fr->addChild(format_btn);
                fr->addChild(eject_btn);
            };

            *show_mount_footer = [footer_row_ptr,
                                  mount_lbl, mount_input, mount_confirm_btn, mount_cancel_btn]() {
                auto fr = *footer_row_ptr;
                if (!fr) return;
                fr->clearChildren();
                fr->addChild(mount_lbl);
                fr->addChild(mount_input);
                fr->addChild(mount_confirm_btn);
                fr->addChild(mount_cancel_btn);
            };

            *show_format_footer = [footer_row_ptr,
                                   format_lbl, fs_ufs_btn, fs_fat_btn,
                                   label_input, format_confirm_btn, format_cancel_btn]() {
                auto fr = *footer_row_ptr;
                if (!fr) return;
                fr->clearChildren();
                fr->addChild(format_lbl);
                fr->addChild(fs_ufs_btn);
                fr->addChild(fs_fat_btn);
                fr->addChild(label_input);
                fr->addChild(format_confirm_btn);
                fr->addChild(format_cancel_btn);
            };

            // ── Assemble root ────────────────────────────────────────────────
            root_col->addChild(title_row);
            root_col->addChild(std::make_shared<DividerWidget>(1.0f));
            root_col->addChild(main_row);
            root_col->addChild(std::make_shared<DividerWidget>(1.0f));
            root_col->addChild(footer_row);

            // ── refresh_partitions lambda ─────────────────────────────────────
            *refresh_partitions = [selected_disk, selected_partition, selected_mounted,
                                   section_hdr_ptr, part_col_ptr, set_status]() {
                if (selected_disk->empty()) return;
                if (!g_xbus) return;

                auto col = *part_col_ptr;
                if (!col) return;
                col->clearChildren();

                // Request partition list (up to 32 pages of 1 each)
                auto disk_name = *selected_disk;
                auto parts = std::make_shared<std::vector<PartitionInfo>>();

                auto fetch_page = std::make_shared<std::function<void(uint32_t)>>();
                *fetch_page = [disk_name, part_col_ptr, selected_partition,
                               selected_mounted, parts, fetch_page](uint32_t page) {
                    struct Req { char disk[32]; uint32_t page; } req{};
                    std::strncpy(req.disk, disk_name.c_str(), sizeof(req.disk) - 1);
                    req.page = page;
                    g_xbus->Call("com.freebsdx.Disk",
                        static_cast<uint32_t>(DiskOpcode::ListPartitions),
                        {reinterpret_cast<const uint8_t*>(&req), sizeof(req)},
                        [part_col_ptr, selected_partition, selected_mounted,
                         parts, fetch_page, page](const XBusMessage& r) {
                            PartitionListPage pg{};
                            std::memcpy(&pg, r.payload_, sizeof(pg));
                            PartitionInfo& p = pg.partition_;
                            p.name_[sizeof(p.name_)-1]='\0';
                            p.fs_type_[sizeof(p.fs_type_)-1]='\0';
                            p.mount_point_[sizeof(p.mount_point_)-1]='\0';
                            p.label_[sizeof(p.label_)-1]='\0';
                            parts->push_back(p);
                            if (pg.has_more_) { if (*fetch_page) (*fetch_page)(page + 1); return; }

                            // Last page — render the whole partition set as one TableWidget.
                            auto col = *part_col_ptr;
                            if (!col) return;
                            col->clearChildren();

                            std::vector<TableColumn> columns{
                                {"Device", 130.0f}, {"FS", 70.0f},
                                {"Size", 84.0f, CellAlign::Right}, {"Used", 60.0f, CellAlign::Right},
                                {"Mount point", 170.0f}, {"Label", 110.0f}};
                            std::vector<std::vector<std::string>> rows;
                            rows.reserve(parts->size());
                            for (const PartitionInfo& pi : *parts) {
                                char used_buf[16] = "?";
                                if (pi.size_bytes_ > 0 && pi.used_bytes_ <= pi.size_bytes_)
                                    std::snprintf(used_buf, sizeof(used_buf), "%llu%%",
                                        (unsigned long long)(100 * pi.used_bytes_ / pi.size_bytes_));
                                rows.push_back({
                                    pi.name_, pi.fs_type_, fmtBytes(pi.size_bytes_), used_buf,
                                    pi.is_mounted_ ? std::string(pi.mount_point_) : std::string("—"),
                                    pi.label_});
                            }

                            auto table = std::make_shared<TableWidget>(
                                std::move(columns), std::move(rows),
                                [parts, selected_partition, selected_mounted](int i) {
                                    if (i < 0 || i >= static_cast<int>(parts->size())) return;
                                    const PartitionInfo& pi = (*parts)[static_cast<size_t>(i)];
                                    *selected_partition = pi.name_;
                                    *selected_mounted   = pi.is_mounted_ != 0;
                                });
                            table->setSortable(false);
                            for (size_t i = 0; i < parts->size(); ++i)
                                if ((*parts)[i].name_ == *selected_partition) {
                                    table->setSelectedRow(static_cast<int>(i)); break; }
                            col->addChild(table);
                        });
                };
                (*fetch_page)(0);

                // Update section header
                if (*section_hdr_ptr)
                    (*section_hdr_ptr)->setText("Partitions — " + disk_name);
            };

            // ── refresh_all lambda ────────────────────────────────────────────
            *refresh_all = [disk_col_ptr, pool_col_ptr,
                            selected_disk, selected_partition, selected_mounted,
                            selected_removable, refresh_partitions, set_status]() {
                if (!g_xbus) return;

                // ---- Disk list ----
                auto dc = *disk_col_ptr;
                if (dc) dc->clearChildren();

                auto fetch_disk = std::make_shared<std::function<void(uint32_t)>>();
                *fetch_disk = [disk_col_ptr, selected_disk, selected_partition,
                               selected_mounted, selected_removable,
                               refresh_partitions, fetch_disk](uint32_t page) {
                    uint32_t pg_idx = page;
                    g_xbus->Call("com.freebsdx.Disk",
                        static_cast<uint32_t>(DiskOpcode::ListDisks),
                        {reinterpret_cast<const uint8_t*>(&pg_idx), sizeof(pg_idx)},
                        [disk_col_ptr, selected_disk, selected_partition,
                         selected_mounted, selected_removable,
                         refresh_partitions, fetch_disk, page](const XBusMessage& r) {
                            DiskListPage pg{};
                            std::memcpy(&pg, r.payload_, sizeof(pg));

                            auto dc2 = *disk_col_ptr;
                            if (!dc2) return;

                            DiskInfo& d = pg.disk_;
                            d.name_[sizeof(d.name_)-1]  = '\0';
                            d.model_[sizeof(d.model_)-1] = '\0';

                            std::string dname = d.name_;
                            bool is_removable = d.is_removable_ != 0;
                            bool is_sel = (dname == *selected_disk);

                            char size_buf[24];
                            std::snprintf(size_buf, sizeof(size_buf), "%s",
                                fmtBytes(d.size_bytes_).c_str());

                            std::string model = d.model_;
                            if (model.size() > 22) model = model.substr(0, 22);

                            std::string line = dname + "  " + size_buf;
                            if (!model.empty()) line += "  " + model;

                            auto name_lbl = std::make_shared<TextWidget>(dname, 13.0f);
                            name_lbl->setColor(Color{0.92f, 0.92f, 0.95f, 1.0f});

                            auto info_lbl = std::make_shared<TextWidget>(
                                std::string(size_buf) + (is_removable ? "  ⏏" : ""), 11.0f);
                            info_lbl->setColor(Color{0.55f, 0.55f, 0.60f, 1.0f});

                            auto model_lbl = std::make_shared<TextWidget>(model, 10.0f);
                            model_lbl->setColor(Color{0.45f, 0.45f, 0.50f, 1.0f});

                            auto entry_col = std::make_shared<FlexColumn>(
                                std::vector<std::shared_ptr<Widget>>{ name_lbl, info_lbl, model_lbl });
                            entry_col->setGap(2.0f);

                            auto entry_btn = std::make_shared<ButtonWidget>(
                                entry_col,
                                [dname, is_removable, selected_disk, selected_partition,
                                 selected_mounted, selected_removable,
                                 refresh_partitions, disk_col_ptr]() {
                                    *selected_disk      = dname;
                                    *selected_partition = "";
                                    *selected_mounted   = false;
                                    *selected_removable = is_removable;
                                    if (*refresh_partitions) (*refresh_partitions)();
                                    // Update selection highlight
                                    auto dc3 = *disk_col_ptr;
                                    if (!dc3) return;
                                    for (size_t i = 0; i < dc3->childCount(); ++i) {
                                        auto btn = std::dynamic_pointer_cast<ButtonWidget>(
                                            dc3->childAt(i));
                                        if (btn) {
                                            auto inner = std::dynamic_pointer_cast<FlexColumn>(
                                                btn->getContent());
                                            if (inner && inner->childCount() > 0) {
                                                auto top = std::dynamic_pointer_cast<TextWidget>(
                                                    inner->childAt(0));
                                                if (top) {
                                                    bool sel = (top->getText() == dname);
                                                    btn->setBackgroundColor(
                                                        sel ? Color{0.18f, 0.30f, 0.48f, 1.0f}
                                                            : Color{0.13f, 0.13f, 0.15f, 1.0f});
                                                }
                                            }
                                        }
                                    }
                                });
                            entry_btn->setPadding(8.0f);
                            entry_btn->setFixedSize(0.0f, 56.0f);
                            entry_btn->setBackgroundColor(
                                is_sel ? Color{0.18f, 0.30f, 0.48f, 1.0f}
                                       : Color{0.13f, 0.13f, 0.15f, 1.0f});
                            dc2->addChild(entry_btn);

                            if (pg.has_more_) {
                                if (*fetch_disk) (*fetch_disk)(page + 1);
                            }
                        });
                };
                (*fetch_disk)(0);

                // ---- ZFS pool list ----
                auto pc = *pool_col_ptr;
                if (pc) pc->clearChildren();

                auto fetch_pool = std::make_shared<std::function<void(uint32_t)>>();
                *fetch_pool = [pool_col_ptr, fetch_pool](uint32_t page) {
                    uint32_t pg_idx = page;
                    g_xbus->Call("com.freebsdx.Disk",
                        static_cast<uint32_t>(DiskOpcode::ListZpools),
                        {reinterpret_cast<const uint8_t*>(&pg_idx), sizeof(pg_idx)},
                        [pool_col_ptr, fetch_pool, page](const XBusMessage& r) {
                            ZpoolListPage pg{};
                            std::memcpy(&pg, r.payload_, sizeof(pg));

                            auto pc2 = *pool_col_ptr;
                            if (!pc2) return;

                            ZpoolInfo& z = pg.pool_;
                            z.name_[sizeof(z.name_)-1]     = '\0';
                            z.health_[sizeof(z.health_)-1]  = '\0';

                            std::string zname  = z.name_;
                            std::string health = z.health_;
                            Color hcol = (health == "ONLINE")
                                ? Color{0.35f, 0.75f, 0.45f, 1.0f}
                                : Color{0.85f, 0.55f, 0.20f, 1.0f};

                            auto n_lbl = std::make_shared<TextWidget>(zname, 12.0f);
                            n_lbl->setColor(Color{0.88f, 0.88f, 0.88f, 1.0f});
                            n_lbl->setFlexGrow(1.0f);

                            auto h_lbl = std::make_shared<TextWidget>(health, 11.0f);
                            h_lbl->setColor(hcol);

                            char sz_buf[24];
                            std::snprintf(sz_buf, sizeof(sz_buf), "%s",
                                fmtBytes(z.size_bytes_).c_str());
                            auto s_lbl = std::make_shared<TextWidget>(sz_buf, 11.0f);
                            s_lbl->setColor(Color{0.55f, 0.55f, 0.60f, 1.0f});

                            auto pool_row = std::make_shared<FlexRow>(
                                std::vector<std::shared_ptr<Widget>>{ n_lbl, s_lbl, h_lbl });
                            pool_row->setCrossAxisAlignment(CrossAxisAlignment::Center);
                            pool_row->setFixedSize(0.0f, 28.0f);
                            pool_row->setPadding(6.0f);
                            pool_row->setGap(6.0f);
                            pool_row->setBackgroundColor(Color{0.13f, 0.13f, 0.15f, 1.0f});
                            pc2->addChild(pool_row);

                            if (pg.has_more_) {
                                if (*fetch_pool) (*fetch_pool)(page + 1);
                            }
                        });
                };
                (*fetch_pool)(0);

                // Refresh current disk's partitions if one is selected
                if (!selected_disk->empty() && *refresh_partitions)
                    (*refresh_partitions)();

                set_status("Refreshed.");
            };

            // ── Initial load ──────────────────────────────────────────────────
            if (*refresh_all) (*refresh_all)();

            // ── XBus broadcast: DiskChanged ───────────────────────────────────
            xbus.OnBroadcast([refresh_all](const XBusMessage& msg) {
                if (std::strcmp(msg.service_name_, "com.freebsdx.Disk") == 0 &&
                    msg.opcode_ == static_cast<uint32_t>(DiskOpcode::DiskChanged)) {
                    if (*refresh_all) (*refresh_all)();
                }
            });
        });
}
