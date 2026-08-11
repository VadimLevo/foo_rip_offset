#include <foobar2000/SDK/foobar2000.h>
#include <foobar2000/helpers/helpers.h>
#include <fstream>
#include <vector>
#include <string>
#include <filesystem>
#include "OffsetDialog.h"

// --- Декларация компонента ---
DECLARE_COMPONENT_VERSION(
    "AccurateRip Offset Fixer",
    "1.0",
    "Bit-perfect sample shifting across track boundaries for AccurateRip correction."
);

VALIDATE_COMPONENT_FILENAME("foo_rip_offset.dll");


// --- Класс для записи WAV ---
class WavWriter {
public:
    bool open(const std::wstring& path, int sample_rate, int channels, int bps = 16) {
        m_file.open(path, std::ios::binary);
        if (!m_file.is_open()) return false;

        m_channels = channels;

        memcpy(m_header.riff, "RIFF", 4);
        memcpy(m_header.wave, "WAVE", 4);
        memcpy(m_header.fmt, "fmt ", 4);
        m_header.fmt_size = 16;
        m_header.audio_format = 1; // 1 = PCM
        m_header.channels = channels;
        m_header.sample_rate = sample_rate;
        m_header.byte_rate = sample_rate * channels * (bps / 8);
        m_header.block_align = channels * (bps / 8);
        m_header.bits_per_sample = bps;
        memcpy(m_header.data, "data", 4);
        m_header.data_size = 0;
        m_header.chunk_size = 0;

        m_file.write(reinterpret_cast<const char*>(&m_header), sizeof(m_header));
        return true;
    }

    void write_samples(const audio_sample* samples, size_t sample_count) {
        if (!m_file.is_open() || sample_count == 0) return;

        size_t total_floats = sample_count * m_channels;
        std::vector<int16_t> int_buffer(total_floats);

        audio_math::convert_to_int16(samples, total_floats, int_buffer.data(), 1.0);

        size_t bytes = total_floats * sizeof(int16_t);
        m_file.write(reinterpret_cast<const char*>(int_buffer.data()), bytes);
        m_data_bytes += bytes;
    }

    void close() {
        if (!m_file.is_open()) return;

        m_header.data_size = m_data_bytes;
        m_header.chunk_size = 36 + m_data_bytes;

        m_file.seekp(0, std::ios::beg);
        m_file.write(reinterpret_cast<const char*>(&m_header), sizeof(m_header));
        m_file.close();
    }

    ~WavWriter() { close(); }

private:
#pragma pack(push, 1)
    struct Header {
        char riff[4];
        uint32_t chunk_size;
        char wave[4];
        char fmt[4];
        uint32_t fmt_size;
        uint16_t audio_format;
        uint16_t channels;
        uint32_t sample_rate;
        uint32_t byte_rate;
        uint16_t block_align;
        uint16_t bits_per_sample;
        char data[4];
        uint32_t data_size;
    } m_header = {};
#pragma pack(pop)

    std::ofstream m_file;
    uint32_t m_data_bytes = 0;
    int m_channels = 0;
};


// --- Вспомогательная функция чтения сэмплов из начала трека ---
static bool read_first_samples(const service_ptr_t<metadb_handle>& item, size_t count_samples, std::vector<audio_sample>& out_buffer, abort_callback& p_abort) {
    input_helper ih;
    ih.open(nullptr, item, input_flag_no_looping, p_abort, false, false);

    audio_chunk_impl chunk;
    size_t collected = 0;

    while (ih.run(chunk, p_abort) && collected < count_samples) {
        size_t channels = chunk.get_channels();
        size_t available = chunk.get_sample_count();
        size_t needed = count_samples - collected;
        size_t to_take = (available < needed) ? available : needed;

        const audio_sample* data = chunk.get_data();
        out_buffer.insert(out_buffer.end(), data, data + (to_take * channels));
        collected += to_take;
    }
    ih.close();
    return collected == count_samples;
}


// --- Фоновый процесс обработки ---
class offset_process : public threaded_process_callback {
public:
    offset_process(metadb_handle_list_cref items, int offset_samples,
        bool same_as_source, bool create_subfolder,
        const char* subfolder_name, const char* custom_path)
        : m_items(items), m_offset(offset_samples),
        m_same_as_source(same_as_source), m_create_subfolder(create_subfolder),
        m_subfolder_name(subfolder_name), m_custom_path(custom_path) {
    }

    void on_init(HWND p_wnd) override {}

    void run(threaded_process_status& p_status, abort_callback& p_abort) override {
        p_status.set_title("Applying AccurateRip Offset...");

        input_helper ih;
        t_size total_items = m_items.get_count();

        std::vector<audio_sample> saved_tail;
        size_t album_channels = 0;
        size_t album_sample_rate = 0;

        for (t_size i = 0; i < total_items; ++i) {
            p_abort.check();
            p_status.set_progress(i, total_items);
            p_status.set_item_path(m_items[i]->get_path());

            // 1. Декодируем весь текущий трек в память
            ih.open(nullptr, m_items[i], input_flag_no_looping, p_abort, false, false);

            audio_chunk_impl chunk;
            std::vector<audio_sample> current_track_data;

            bool format_initialized = false;
            size_t channels = 0;
            size_t sample_rate = 0;

            while (ih.run(chunk, p_abort)) {
                if (!format_initialized) {
                    channels = chunk.get_channels();
                    sample_rate = chunk.get_srate();
                    if (i == 0) {
                        album_channels = channels;
                        album_sample_rate = sample_rate;
                    }
                    format_initialized = true;
                }
                const audio_sample* data = chunk.get_data();
                size_t total_samples = chunk.get_sample_count() * channels;
                current_track_data.insert(current_track_data.end(), data, data + total_samples);
            }
            ih.close();

            size_t total_frames = current_track_data.size() / channels;

            // 2. Формируем путь вывода
            pfc::string8 in_path = m_items[i]->get_path();
            if (strncmp(in_path.get_ptr(), "file://", 7) == 0) in_path.remove_chars(0, 7);

            pfc::stringcvt::string_wide_from_utf8 w_in_path(in_path.get_ptr());
            std::filesystem::path orig_path(w_in_path.get_ptr());
            std::filesystem::path out_dir;

            if (m_same_as_source) {
                out_dir = orig_path.parent_path();
            }
            else {
                pfc::stringcvt::string_wide_from_utf8 w_custom(m_custom_path.get_ptr());
                out_dir = std::filesystem::path(w_custom.get_ptr());
            }

            if (m_create_subfolder && !m_subfolder_name.is_empty()) {
                pfc::stringcvt::string_wide_from_utf8 w_sub(m_subfolder_name.get_ptr());
                out_dir /= w_sub.get_ptr();
            }

            std::error_code ec;
            std::filesystem::create_directories(out_dir, ec);

            std::filesystem::path out_file = out_dir / orig_path.filename();
            std::wstring final_path_str = out_file.wstring() + L".fixed.wav";

            WavWriter writer;
            writer.open(final_path_str, sample_rate, channels);

            // 3. Применяем математику сдвига
            if (m_offset < 0) {
                size_t skip_frames = static_cast<size_t>(-m_offset);
                size_t borrow_frames = skip_frames;

                if (skip_frames < total_frames) {
                    size_t offset_samples = skip_frames * channels;
                    writer.write_samples(current_track_data.data() + offset_samples, total_frames - skip_frames);
                }

                if (i + 1 < total_items) {
                    std::vector<audio_sample> borrowed;
                    read_first_samples(m_items[i + 1], borrow_frames, borrowed, p_abort);
                    writer.write_samples(borrowed.data(), borrow_frames);
                }
                else {
                    std::vector<audio_sample> silence(borrow_frames * channels, 0.0f);
                    writer.write_samples(silence.data(), borrow_frames);
                }

            }
            else if (m_offset > 0) {
                size_t shift_frames = static_cast<size_t>(m_offset);
                size_t shift_samples = shift_frames * channels;

                if (i == 0) {
                    std::vector<audio_sample> silence(shift_samples, 0.0f);
                    writer.write_samples(silence.data(), shift_frames);
                }
                else {
                    writer.write_samples(saved_tail.data(), saved_tail.size() / channels);
                }

                if (total_frames > shift_frames) {
                    size_t body_frames = total_frames - shift_frames;
                    writer.write_samples(current_track_data.data(), body_frames);

                    size_t tail_start_sample = body_frames * channels;
                    saved_tail.assign(current_track_data.begin() + tail_start_sample, current_track_data.end());
                }
                else {
                    writer.write_samples(current_track_data.data(), total_frames);
                    saved_tail.assign(current_track_data.begin(), current_track_data.end());
                }

            }
            else {
                writer.write_samples(current_track_data.data(), total_frames);
            }

            writer.close();

            pfc::string8 out_url = "file://";
            pfc::stringcvt::string_utf8_from_wide utf8_final_path(final_path_str.c_str());
            out_url += utf8_final_path;

            // 4. Копируем текстовые теги
            file_info_impl info;
            if (m_items[i]->get_info(info)) {
                try {
                    service_ptr_t<input_info_writer> p_writer;
                    input_entry::g_open_for_info_write(p_writer, nullptr, out_url.get_ptr(), p_abort);
                    p_writer->set_info(0, info, p_abort);
                    p_writer->commit(p_abort);
                }
                catch (const std::exception& e) {
                    FB2K_console_formatter() << "foo_rip_offset: Failed to write tags for " << out_url << " - " << e.what();
                }
            }

            // 5. Копируем вшитую обложку (Album Art)
            pfc::string8 src_url = m_items[i]->get_path();
            try {
                // g_open первым аргументом принимает file_ptr (передаём nullptr),
                // а возвращает готовый указатель на экстрактор/редактор.
                album_art_extractor_instance_ptr extractor = album_art_extractor::g_open(nullptr, src_url.get_ptr(), p_abort);

                album_art_editor_instance_ptr editor = album_art_editor::g_open(nullptr, out_url.get_ptr(), p_abort);

                static const GUID* const art_types[] = {
                    &album_art_ids::cover_front,
                    &album_art_ids::cover_back,
                    &album_art_ids::disc,
                    &album_art_ids::artist
                };

                bool modified = false;
                for (const GUID* type_guid : art_types) {
                    try {
                        album_art_data_ptr data = extractor->query(*type_guid, p_abort);
                        if (data.is_valid()) {
                            editor->set(*type_guid, data, p_abort);
                            modified = true;
                        }
                    }
                    catch (const exception_album_art_not_found&) {
                        // Нет конкретного типа обложки - это нормально, идём дальше
                    }
                    catch (...) {}
                }

                if (modified) {
                    editor->commit(p_abort);
                }
            }
            catch (const exception_album_art_not_found&) {
                // У исходного файла вообще нет обложек - штатная ситуация
            }
            catch (const exception_album_art_unsupported_format&) {
                // Исходный или целевой формат не поддерживает работу с обложками
            }
            catch (const std::exception& e) {
                FB2K_console_formatter() << "foo_rip_offset: Failed to copy album art - " << e.what();
            }
        }
    }

    void on_done(HWND p_wnd, bool p_was_aborted) override {
        if (!p_was_aborted) {
            popup_message::g_show("Offset processing completed successfully!", "Done");
        }
        else {
            FB2K_console_formatter() << "foo_rip_offset: Operation aborted by user.";
        }
    }

private:
    metadb_handle_list m_items;
    int m_offset;
    bool m_same_as_source;
    bool m_create_subfolder;
    pfc::string8 m_subfolder_name;
    pfc::string8 m_custom_path;
};


// --- Контекстное меню ---
class contextmenu_offset_fix : public contextmenu_item_simple {
public:
    enum {
        cmd_fix_offset = 0,
        cmd_total
    };

    unsigned get_num_items() override {
        return cmd_total;
    }

    void get_item_name(unsigned p_index, pfc::string_base& p_out) override {
        switch (p_index) {
        case cmd_fix_offset: p_out = "Apply Rip Offset..."; break;
        }
    }

    bool context_get_display(unsigned p_index, metadb_handle_list_cref p_data, pfc::string_base& p_out, unsigned& p_displayflags, const GUID& p_caller) override {
        get_item_name(p_index, p_out);
        if (p_data.get_count() == 0) {
            p_displayflags |= FLAG_DISABLED;
        }
        return true;
    }

    void context_command(unsigned p_index, metadb_handle_list_cref p_data, const GUID& p_caller) override {
        switch (p_index) {
        case cmd_fix_offset:
        {
            if (p_data.get_count() == 0) return;

            COffsetDialog dlg;
            HWND parent_window = core_api::get_main_window();

            if (dlg.DoModal(parent_window) == IDOK) {
                pfc::stringcvt::string_utf8_from_os utf8_subfolder(dlg.m_subfolder_name);
                pfc::stringcvt::string_utf8_from_os utf8_custom_path(dlg.m_custom_path);

                service_ptr_t<offset_process> cb = new service_impl_t<offset_process>(
                    p_data,
                    dlg.m_offset_value,
                    dlg.m_same_as_source,
                    dlg.m_create_subfolder,
                    utf8_subfolder.get_ptr(),
                    utf8_custom_path.get_ptr()
                );

                threaded_process::g_run_modeless(
                    cb,
                    threaded_process::flag_show_progress | threaded_process::flag_show_abort,
                    parent_window,
                    "Offset Fixer"
                );
            }
            break;
        }
        }
    }

    GUID get_item_guid(unsigned p_index) override {
        static const GUID guid_cmd_fix_offset = { 0x4d3b8f1a, 0x2e5c, 0x4a9d, { 0xb1, 0x72, 0x8f, 0x3c, 0x9a, 0x1e, 0x5b, 0x2d } };
        switch (p_index) {
        case cmd_fix_offset: return guid_cmd_fix_offset;
        default: uBugCheck();
        }
    }

    bool get_item_description(unsigned p_index, pfc::string_base& p_out) override {
        switch (p_index) {
        case cmd_fix_offset:
            p_out = "Applies bit-perfect sample shift across selected tracks and saves to specified folder.";
            return true;
        default:
            return false;
        }
    }
};

static contextmenu_item_factory_t<contextmenu_offset_fix> g_contextmenu_offset_fix_factory;