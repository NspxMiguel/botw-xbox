// SPDX-License-Identifier: MIT
// App members; disk, HTTP, decoding and archive work always run on a worker.
mods::store::Fetch store_fetcher() {
    return [requests = store_requests](const std::string &url) {
        auto request = requests->start();
        std::atomic<bool> timed_out{false};
        uwp::RequestDeadline deadline(std::chrono::seconds(45), [request, &timed_out] {
            timed_out = true;
            winrt::init_apartment(winrt::apartment_type::multi_threaded);
            request->cancel();
            winrt::uninit_apartment();
        });
        try {
            auto response = uwp::fetch_json(url, *request).get();
            request->check();
            return to_string(response);
        } catch (mods::store::NetworkError const &) {
            request->check();
            throw;
        } catch (winrt::hresult_canceled const &) {
            if (timed_out && !requests->is_closed())
                throw mods::store::NetworkError("Request timed out");
            throw;
        } catch (winrt::hresult_error const &e) {
            request->check();
            throw mods::store::NetworkError(to_string(e.message()));
        }
    };
}
void store_filter() {
    auto &view = home.store;
    auto query = view.source == 3 ? std::string{} : to_string(view.query);
    view.cards.clear();
    if (view.tab == 1) {
        for (auto c : store_installed)
            if ((view.source == 0 || (view.source == 1 && c.source == "GameBanana") ||
                 (view.source == 2 && c.source == "Graphic packs") ||
                 (view.source == 3 && c.source == "Link") || (view.source == 4 && c.source == "USB")) &&
                mods::store::matches(c, query, static_cast<mods::store::Category>(view.category)))
                view.cards.push_back(std::move(c));
    } else {
        for (auto c : store_available) {
            for (auto &installed : store_installed)
                if (c.id == installed.id && c.source == installed.source) {
                    c.installed_id = installed.installed_id;
                    c.installed_version = installed.version;
                    c.installed_date = installed.date;
                    c.enabled = installed.enabled;
                    c.locked = installed.locked;
                    c.order = installed.order;
                    c.total = installed.total;
                }
            if (mods::store::matches(c, query, static_cast<mods::store::Category>(view.category)) &&
                (view.tab != 2 || c.update()))
                view.cards.push_back(std::move(c));
        }
    }
    if (home.focus >= int(view.cards.size()))
        home.focus = view.cards.empty() ? -20 : 0;
}
IAsyncAction store_load_installed() {
    auto lifetime = get_strong();
    auto dispatcher = host_window.Dispatcher();
    auto root = store_root;
    std::vector<mods::store::Card> cards;
    std::exception_ptr failure;
    co_await resume_background();
    try {
        mods::ModCatalog catalog;
        catalog.load(root, packs::compiled_plan());
        for (auto &entry : catalog.list()) {
            mods::store::Card c;
            c.id = entry.id;
            c.title = entry.name;
            c.source = "USB";
            c.installed_id = entry.id;
            c.enabled = entry.enabled;
            c.locked = entry.locked;
            c.order = cards.size();
            c.total = catalog.list().size();
            c.category = entry.kind == mods::Kind::graphics ? mods::store::Category::graphics
                         : (entry.kind == mods::Kind::translation || entry.kind == mods::Kind::dubbing)
                             ? mods::store::Category::language
                             : mods::store::category(entry.name);
            if (entry.id.starts_with("file:"))
                mods::store::read_receipt(
                    root / std::filesystem::path(to_hstring(entry.id.substr(5)).c_str()), c);
            else
                c.source = "Graphic packs";
            cards.push_back(std::move(c));
        }
    } catch (...) {
        failure = std::current_exception();
    }
    co_await resume_foreground(dispatcher);
    if (failure)
        std::rethrow_exception(failure);
    store_installed = std::move(cards);
    store_filter();
}
IAsyncAction store_refresh(uint64_t generation, int source, std::string query, bool force = false) {
    auto lifetime = get_strong();
    auto dispatcher = host_window.Dispatcher();
    auto root = store_root;
    auto fetch = store_fetcher();
    auto installed = store_installed;
    struct FetchGuard {
        std::atomic<bool> &fetching;
        ~FetchGuard() { fetching = false; }
    } guard{store_fetching};
    struct Result {
        mods::store::Page page;
        bool error = false;
    };
    std::vector<std::future<Result>> pending;
    bool launch_error = false;
    std::wstring recent;
    co_await resume_background();
    try {
        mods::store::remember_search(root, query);
        for (auto &search : mods::store::recent_searches(root))
            recent += std::wstring(to_hstring(search)) + L"   ";
        for (int id = 1; id <= 4; ++id) {
            if ((source != 0 && source != id) || (source == 0 && id == 3))
                continue;
            // At most four source workers, with one request at a time per worker.
            pending.push_back(std::async(std::launch::async, [=] {
                winrt::init_apartment(winrt::apartment_type::multi_threaded);
                struct ApartmentGuard {
                    ~ApartmentGuard() { winrt::uninit_apartment(); }
                } apartment;
                Result out;
                try {
                    std::vector<std::filesystem::path> drives;
                    if (id == 4)
                        for (auto drive : KnownFolders::RemovableDevices().GetFoldersAsync().get())
                            drives.emplace_back(drive.Path().c_str());
                    auto adapter = mods::store::make_source(static_cast<mods::store::SourceId>(id), fetch,
                                                            root, drives, force);
                    for (unsigned page = 1; page <= 4; ++page) {
                        auto result = adapter->search(id == 2 ? "" : query, mods::store::Category::all, page);
                        out.page.offline |= result.offline;
                        out.page.cards.insert(out.page.cards.end(),
                                              std::make_move_iterator(result.cards.begin()),
                                              std::make_move_iterator(result.cards.end()));
                        if (!result.more)
                            break;
                    }
                    for (auto card : installed) {
                        int remote = card.source == "GameBanana" ? 1 : card.source == "Graphic packs" ? 2 : 0;
                        if (remote != id || remote == 2 || card.id.starts_with("pack:") ||
                            std::any_of(out.page.cards.begin(), out.page.cards.end(),
                                        [&](auto &c) { return c.id == card.id && c.source == card.source; }))
                            continue;
                        try {
                            out.page.cards.push_back(adapter->files(card));
                        } catch (...) {
                            out.error = true;
                        }
                    }
                } catch (const winrt::hresult_error &e) {
                    LOG("mods: source %d failed (0x%08lx %ls)", id, static_cast<unsigned long>(e.code().value),
                        e.message().c_str());
                    out.error = true;
                } catch (const std::exception &e) {
                    LOG("mods: source %d failed: %s", id, e.what());
                    out.error = true;
                } catch (...) {
                    LOG("mods: source %d failed", id);
                    out.error = true;
                }
                return out;
            }));
        }
    } catch (...) {
        launch_error = true;
    }
    if (launch_error) {
        co_await resume_foreground(dispatcher);
        if (generation == store_generation) {
            home.store.busy = false;
            home.store.error = true;
        }
        co_await resume_background();
    }
    bool first = true;
    while (!pending.empty()) {
        auto ready = std::find_if(pending.begin(), pending.end(), [](auto &work) {
            return work.wait_for(std::chrono::seconds(0)) == std::future_status::ready;
        });
        if (ready == pending.end()) {
            co_await resume_after(std::chrono::milliseconds(16));
            continue;
        }
        Result result;
        try {
            result = ready->get();
        } catch (...) {
            result.error = true;
        }
        pending.erase(ready);
        co_await resume_foreground(dispatcher);
        if (home.store.active && generation == store_generation) {
            if (first) {
                store_available.clear();
                first = false;
            }
            store_available.insert(store_available.end(), std::make_move_iterator(result.page.cards.begin()),
                                   std::make_move_iterator(result.page.cards.end()));
            home.store.recent = home.store.label(uwp::Text::store_recent) + L" · " + recent;
            home.store.busy = !pending.empty();
            home.store.offline |= result.page.offline;
            home.store.error |= result.error;
            home.store.status = home.store.error ? home.store.label(uwp::Text::store_error) : L"";
            store_filter();
        }
        co_await resume_background();
    }
}

IAsyncAction store_thumbnail(std::string url, uint64_t generation) {
    auto lifetime = get_strong();
    auto dispatcher = host_window.Dispatcher();
    auto root = store_root;
    auto requests = store_requests;
    std::optional<uwp::ArtImage> decoded;
    co_await resume_background();
    try {
        auto hash = packs::sha256_bytes(reinterpret_cast<const uint8_t *>(url.data()), url.size());
        auto folder = root / "cache" / "thumbs";
        std::filesystem::create_directories(folder);
        auto path = folder / hash;
        if (!std::filesystem::exists(path)) {
            auto request = requests->start();
            uwp::RequestDeadline deadline(std::chrono::seconds(20), [request] {
                winrt::init_apartment(winrt::apartment_type::multi_threaded);
                request->cancel();
                winrt::uninit_apartment();
            });
            auto temp = path;
            static std::atomic<uint64_t> next_job{0};
            temp += ".part-" + std::to_string(next_job.fetch_add(1));
            try {
                uwp::download_mod(
                    {"thumbnail", url, "", 0}, temp, *request, [](auto, auto) {}, uwp::art_byte_limit)
                    .get();
                request->check();
                std::filesystem::rename(temp, path);
            } catch (...) {
                std::error_code ec;
                std::filesystem::remove(temp, ec);
                throw;
            }
        }
        std::ifstream in(path, std::ios::binary);
        std::vector<uint8_t> data{std::istreambuf_iterator<char>(in), {}};
        decoded = uwp::decode_store_image(data);
        if (!decoded)
            LOG("mods: thumbnail not decodable (%zu bytes): %s", data.size(), url.c_str());
    } catch (const std::exception &e) {
        LOG("mods: thumbnail failed: %s (%s)", url.c_str(), e.what());
    } catch (...) {
        LOG("mods: thumbnail failed: %s", url.c_str());
    }
    co_await resume_foreground(dispatcher);
    if (decoded && home.store.active && generation == store_generation && home.ready())
        home.store_image(url, *decoded);
}
IAsyncAction store_detail(mods::store::Card card) {
    auto lifetime = get_strong();
    auto dispatcher = host_window.Dispatcher();
    auto root = store_root;
    auto fetch = store_fetcher();
    auto generation = store_generation;
    auto identity = card.id;
    home.store.detail_error = false;
    home.store.detail = card;
    home.store.status = home.store.label(uwp::Text::store_busy);
    home.focus = 0;
    bool error = false;
    co_await resume_background();
    try {
        if (card.source == "GameBanana" || card.source == "Graphic packs") {
            auto id = card.source == "GameBanana" ? mods::store::SourceId::gamebanana
                                                  : mods::store::SourceId::graphics;
            // Compiled packs have no remote identity; their actions remain catalog-only.
            if (!card.id.starts_with("pack:"))
                card = mods::store::make_source(id, fetch, root)->files(card);
        }
    } catch (...) {
        error = true;
    }
    co_await resume_foreground(dispatcher);
    if (!home.store.active || store_generation != generation || !home.store.detail ||
        home.store.detail->id != identity)
        co_return;
    home.store.detail = std::move(card);
    home.store.file = 0;
    home.store.image = 0;
    if (!home.store.action_enabled(home.focus))
        home.store.navigate(2, home.focus);
    home.store.detail_error = error;
    home.store.status = error ? home.store.label(uwp::Text::store_error) : L"";
}
IAsyncAction store_edit(mods::store::Card card, int action) {
    if (card.installed_id.empty())
        co_return;
    auto lifetime = get_strong();
    auto dispatcher = host_window.Dispatcher();
    auto root = store_root;
    bool error = false;
    co_await resume_background();
    try {
        mods::ModCatalog catalog;
        catalog.load(root, packs::compiled_plan());
        auto found = std::find_if(catalog.list().begin(), catalog.list().end(),
                                  [&](auto &m) { return m.id == card.installed_id; });
        if (found == catalog.list().end())
            throw std::runtime_error("Missing installed mod");
        if (action == 1) {
            if (!catalog.setEnabled(found->id, !found->enabled))
                throw std::runtime_error("Locked pack");
        }
        if (action == 3 || action == 4) {
            auto index = size_t(found - catalog.list().begin());
            catalog.move(found->id, action == 3 ? (index ? index - 1 : 0)
                                                : std::min(index + 1, catalog.list().size() - 1));
        }
        if (action == 2) {
            if (!card.installed_id.starts_with("file:"))
                throw std::runtime_error("Compiled pack cannot be removed");
            auto directory = root / std::filesystem::path(to_hstring(card.installed_id.substr(5)).c_str());
            if (directory.parent_path() != root || std::filesystem::is_symlink(directory))
                throw std::runtime_error("Invalid installed path");
            catalog.setEnabled(card.installed_id, false);
            catalog.save();
            std::filesystem::remove_all(directory);
            catalog.load(root, packs::compiled_plan());
        }
        catalog.save();
    } catch (...) {
        error = true;
    }
    co_await resume_foreground(dispatcher);
    co_await store_load_installed();
    home.store.status = error ? home.store.label(uwp::Text::import_failed) : L"";
    if (home.store.detail) {
        for (auto &c : store_installed)
            if (c.installed_id == card.installed_id) {
                home.store.detail->enabled = c.enabled;
                home.store.detail->order = c.order;
                home.store.detail->total = c.total;
                break;
            }
        if (action == 2 && !error)
            home.store.detail.reset();
    }
}
IAsyncAction store_install(mods::store::Card card) {
    auto lifetime = get_strong();
    auto &view = home.store;
    if (card.downloads.empty()) {
        if (card.source != "USB" || !card.installed_id.empty())
            co_return;
        auto dispatcher = host_window.Dispatcher();
        auto root = store_root;
        bool failed = false;
        auto destination = co_await local.GetFolderAsync(L"mods");
        co_await resume_background();
        StorageFolder target{nullptr};
        try {
            auto source = StorageFolder::GetFolderFromPathAsync(to_hstring(card.id)).get();
            target =
                destination
                    .CreateFolderAsync(to_hstring(card.title), CreationCollisionOption::GenerateUniqueName)
                    .get();
            if (!copy_mod_tree(source, target))
                throw std::runtime_error("Empty mod");
            auto path = std::filesystem::path(target.Path().c_str());
            if (std::filesystem::exists(path / "rules.txt")) {
                std::ofstream marker(path / ".imported-graphics");
                marker << "Recompile to apply this graphic pack.\n";
            }
            mods::store::save_receipt(path, card);
        } catch (...) {
            failed = true;
            if (target) {
                try {
                    target.DeleteAsync(StorageDeleteOption::PermanentDelete).get();
                } catch (...) {
                }
            }
        }
        co_await resume_foreground(dispatcher);
        view.status = view.label(failed ? uwp::Text::import_failed : uwp::Text::installed);
        co_await store_load_installed();
        co_return;
    }
    auto file = card.downloads[size_t(std::clamp(view.file, 0, int(card.downloads.size()) - 1))];
    if (!mods::supported_archive(file.name)) {
        view.status = view.label(uwp::Text::unsupported);
        co_return;
    }
    auto dispatcher = host_window.Dispatcher();
    auto root = store_root;
    bool error = false;
    auto extension = std::filesystem::path(to_hstring(file.name).c_str()).extension();
    auto temporary = root / ".downloads" / (L"store" + extension.wstring());
    transfer = {};
    transfer_active = true;
    view.progress = 0;
    view.status = view.label(uwp::Text::download);
    co_await resume_background();
    try {
        std::filesystem::create_directories(temporary.parent_path());
        if (card.source == "USB")
            std::filesystem::copy_file(std::filesystem::path(to_hstring(file.url).c_str()), temporary,
                                       std::filesystem::copy_options::overwrite_existing);
        else {
            uwp::download_mod(file, temporary, transfer, [this, dispatcher](uint64_t n, uint64_t total) {
                dispatcher.RunAsync(CoreDispatcherPriority::Normal, [this, n, total] {
                    if (home.store.active && transfer_active)
                        home.store.progress = total ? float(double(n) / double(total)) : 0;
                });
            }).get();
        }
        if (transfer.cancelled)
            throw std::runtime_error("Cancelled");
        auto result =
            mods::install_archive(temporary, root, card.title, card.source == "Graphic packs" ? card.id : "");
        mods::store::finish_install(result.path, [&] {
            mods::store::save_receipt(result.path, card);
            // Keep the old version until the new archive is fully validated and extracted.
            if (!card.installed_id.empty() && card.installed_id.starts_with("file:")) {
                mods::ModCatalog catalog;
                catalog.load(root, packs::compiled_plan());
                auto old = root / std::filesystem::path(to_hstring(card.installed_id.substr(5)).c_str());
                auto name = to_string(result.path.filename().wstring());
                auto new_id = "file:" + name;
                size_t index = 0;
                for (auto &m : catalog.list()) {
                    if (m.id == card.installed_id)
                        break;
                    ++index;
                }
                catalog.setEnabled(card.installed_id, false);
                catalog.setEnabled(new_id, card.enabled);
                catalog.move(new_id, std::min(index, catalog.list().size() - 1));
                catalog.save();
                if (old.parent_path() == root && !std::filesystem::is_symlink(old)) {
                    std::error_code cleanup;
                    std::filesystem::remove_all(old, cleanup);
                }
            }
        });
    } catch (...) {
        error = true;
    }
    std::error_code ec;
    std::filesystem::remove(temporary, ec);
    co_await resume_foreground(dispatcher);
    transfer_active = false;
    transfer.set_pending(nullptr);
    view.progress = -1;
    view.status = view.label(error ? uwp::Text::online_error : uwp::Text::installed);
    co_await store_load_installed();
    if (view.detail)
        for (auto &c : store_installed)
            if (c.id == card.id && c.source == card.source) {
                view.detail->installed_id = c.installed_id;
                view.detail->installed_version = c.version;
                view.detail->installed_date = c.date;
                view.detail->enabled = c.enabled;
                view.detail->locked = c.locked;
            }
}
IAsyncAction manage_mods() {
    auto lifetime = get_strong();
    if (running) {
        co_await manage_mods_legacy();
        co_return;
    }
    auto folder = co_await local.CreateFolderAsync(L"mods", CreationCollisionOption::OpenIfExists);
    store_root = std::filesystem::path(folder.Path().c_str());
    try {
        show_home();
    } catch (...) {
        home.close();
    }
    if (!home.ready()) {
        co_await manage_mods_legacy();
        co_return;
    }
    store_requests = std::make_shared<uwp::StoreRequests>();
    mods_open = true;
    mods_action = 0;
    home.mods_mode = true;
    home.store = {};
    home.store.active = true;
    home.store.pt = pt;
    struct Guard {
        App &app;
        ~Guard() {
            app.store_requests->cancel();
            app.transfer.cancel();
            ++app.store_generation;
            app.home.store.active = false;
            app.mods_open = false;
            app.home.mods_mode = false;
            app.home.shown = false;
            app.end_text_entry();
        }
    } guard{*this};
    home.focus = 0;
    home.selected = -1;
    store_repeat = {};
    store_available.clear();
    co_await store_load_installed();
    store_request = true;
    auto dispatcher = host_window.Dispatcher();
    IAsyncAction refresh{nullptr}, detail_work{nullptr};
    std::vector<IAsyncAction> thumbs;
    bool force_refresh = false;
    std::set<std::string> attempted;
    std::wstring previous_query;
    auto changed = std::chrono::steady_clock::now();
    for (;;) {
        co_await resume_after(std::chrono::milliseconds(16));
        co_await resume_foreground(dispatcher);
        if (quit || !home.ready())
            co_return;
        auto &v = home.store;
        if (v.query != previous_query) {
            previous_query = v.query;
            changed = std::chrono::steady_clock::now();
            store_request = true;
            ++store_generation;
        }
        if (store_request && !store_fetching &&
            std::chrono::steady_clock::now() - changed >= std::chrono::milliseconds(320)) {
            store_request = false;
            store_fetching = true;
            v.busy = true;
            v.error = false;
            v.offline = false;
            refresh = store_refresh(++store_generation, v.source, to_string(v.query), force_refresh);
            force_refresh = false;
        }
        std::erase_if(thumbs, [](auto &work) { return work.Status() != AsyncStatus::Started; });
        if (!store_request && thumbs.size() < 3) {
            std::vector<std::string> images;
            if (v.detail)
                images = v.detail->images;
            else {
                auto first = v.first(home.focus);
                for (size_t i = first; i < v.cards.size() && i < first + 6; ++i)
                    if (!v.cards[i].images.empty())
                        images.push_back(v.cards[i].images[0]);
            }
            std::erase_if(attempted, [&](const std::string &url) {
                return std::find(images.begin(), images.end(), url) == images.end();
            });
            for (auto &url : images)
                if (!home.thumbnails.count(url) && attempted.insert(url).second) {
                    thumbs.push_back(store_thumbnail(url, store_generation));
                    if (thumbs.size() == 3)
                        break;
                }
        }
        auto action = std::exchange(mods_action, 0);
        int selected = std::exchange(home.selected, -1);
        if (selected == -70) {
            store_request = true;
            force_refresh = true;
        }
        if (action == 1 && text_entry) {
            end_text_entry();
            continue;
        }
        if (action == 1) {
            if (v.detail) {
                v.detail.reset();
                home.focus = 0;
            } else
                co_return;
            continue;
        }
        if (action == 3 || selected == -50) {
            // Keep the catalog on screen while CoreTextEditContext owns keyboard input.
            if (!text_entry) {
                begin_text_entry();
                input_text = v.query;
                int32_t caret = static_cast<int32_t>(input_text.size());
                text_selection = {caret, caret};
                edit_context.NotifyTextChanged({0, 0}, caret, text_selection);
                winrt::Windows::UI::ViewManagement::InputPane::GetForCurrentView().TryShow();
            } else
                end_text_entry();
        }
        if (text_entry) {
            v.query = input_text;
            if (selected >= 0) {
                end_text_entry();
                selected = -1;
            }
            continue;
        }
        if (action == 4 || action == 5) {
            if (v.detail) {
                if (!v.detail->images.empty())
                    v.image = (v.image + (action == 4 ? -1 : 1) + int(v.detail->images.size())) %
                              int(v.detail->images.size());
                continue;
            }
            v.tab = (v.tab + (action == 4 ? 2 : 1)) % 3;
            home.focus = 0;
            store_filter();
        }
        if (action == 6 || action == 7) {
            v.category = (v.category + (action == 6 ? 8 : 1)) % 9;
            home.focus = 0;
            store_filter();
        }
        if (selected <= -60 && selected >= -62) {
            v.tab = -selected - 60;
            home.focus = 0;
            store_filter();
        }
        if (selected <= -40 && selected >= -44) {
            v.source = -selected - 40;
            home.focus = 0;
            store_request = true;
            store_available.clear();
            ++store_generation;
        }
        if (selected <= -20 && selected >= -28) {
            v.category = -selected - 20;
            home.focus = 0;
            store_filter();
            if (v.error || v.source == 4)
                store_request = true;
        }
        bool error = false;
        try {
            if (v.detail) {
                auto card = *v.detail;
                if (selected >= 0 && !v.action_enabled(selected))
                    selected = -1;
                if (action == 2 && !card.installed_id.empty())
                    co_await store_edit(card, 1);
                else if (selected == 0 && v.detail_error)
                    detail_work = store_detail(card);
                else if (selected == 0)
                    co_await store_install(card);
                else if (selected == 1 || selected == 3 || selected == 4)
                    co_await store_edit(card, selected);
                else if (selected == 2) {
                    v.active = false;
                    auto confirm =
                        co_await choose(label(uwp::Text::mods), label(uwp::Text::store_remove_question),
                                        {label(uwp::Text::yes), label(uwp::Text::back)});
                    v.active = true;
                    if (confirm == 0)
                        co_await store_edit(card, 2);
                } else if (selected >= 6) {
                    v.file = selected - 6;
                    home.focus = 0;
                } else if (selected == 5 && !card.downloads.empty())
                    home.focus = 6;
            } else if (action == 2 && home.focus >= 0 && home.focus < int(v.cards.size()) &&
                       !v.cards[size_t(home.focus)].installed_id.empty())
                co_await store_edit(v.cards[size_t(home.focus)], 1);
            else if (selected >= 0 && selected < int(v.cards.size()))
                detail_work = store_detail(v.cards[size_t(selected)]);
            else if (selected >= 0 && v.cards.empty())
                store_request = true;
        } catch (...) {
            error = true;
        }
        if (error)
            v.status = v.label(uwp::Text::online_error);
    }
}
