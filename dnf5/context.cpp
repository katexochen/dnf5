/*
Copyright Contributors to the libdnf project.

This file is part of libdnf: https://github.com/rpm-software-management/libdnf/

Libdnf is free software: you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation, either version 2 of the License, or
(at your option) any later version.

Libdnf is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with libdnf.  If not, see <https://www.gnu.org/licenses/>.
*/

#include "dnf5/context.hpp"

#include "download_callbacks.hpp"
#include "plugins.hpp"
#include "utils/string.hpp"
#include "utils/url.hpp"

#include "libdnf5/utils/fs/file.hpp"

#include <fmt/format.h>
#include <libdnf5-cli/progressbar/multi_progress_bar.hpp>
#include <libdnf5-cli/tty.hpp>
#include <libdnf5-cli/utils/userconfirm.hpp>
#include <libdnf5/base/base.hpp>
#include <libdnf5/base/goal.hpp>
#include <libdnf5/conf/const.hpp>
#include <libdnf5/rpm/package_query.hpp>
#include <libdnf5/rpm/package_set.hpp>
#include <libdnf5/rpm/rpm_signature.hpp>
#include <libdnf5/transaction/offline.hpp>
#include <libdnf5/utils/bgettext/bgettext-lib.h>
#include <libdnf5/utils/bgettext/bgettext-mark-domain.h>
#include <libdnf5/utils/fs/file.hpp>
#include <libdnf5/utils/patterns.hpp>
#include <rpm/rpmtypes.h>

#include <algorithm>
#include <cctype>
#include <iostream>
#include <regex>
#include <set>
#include <string>
#include <tuple>
#include <utility>

namespace fs = std::filesystem;

namespace dnf5 {

namespace {

// The `KeyImportRepoCB` class implements callback only for importing repository key.
class KeyImportRepoCB : public libdnf5::repo::RepoCallbacks2_1 {
public:
    explicit KeyImportRepoCB(libdnf5::ConfigMain & config) : config(&config) {}

    bool repokey_import(const libdnf5::rpm::KeyInfo & key_info) override {
        // TODO(jrohel): In case `assumeno`==true, the key is not imported. Is it OK to skip import attempt information message?
        //               And what about `assumeyes`==true in silent mode? Print key import message or not?
        if (config->get_assumeno_option().get_value()) {
            return false;
        }

        std::cerr << libdnf5::utils::sformat(_("Importing OpenPGP key 0x{}:\n"), key_info.get_short_key_id());
        for (auto & user_id : key_info.get_user_ids()) {
            std::cerr << libdnf5::utils::sformat(_(" UserID     : \"{}\"\n"), user_id);
        }
        std::cerr << libdnf5::utils::sformat(
            _(" Fingerprint: {}\n"
              " From       : {}\n"),
            key_info.get_fingerprint(),
            key_info.get_url());

        return libdnf5::cli::utils::userconfirm::userconfirm(*config);
    }

    void repokey_imported([[maybe_unused]] const libdnf5::rpm::KeyInfo & key_info) override {
        std::cerr << _("The key was successfully imported.") << std::endl;
    }

    bool repokey_remove(const libdnf5::rpm::KeyInfo & key_info, const libdnf5::Message & removal_info) override {
        if (config->get_assumeno_option().get_value()) {
            return false;
        }

        std::cerr << libdnf5::utils::sformat(
                         _("The following OpenPGP key (0x{}) is about to be removed:"), key_info.get_short_key_id())
                  << std::endl;
        std::cerr << libdnf5::utils::sformat(_(" Reason     : {}\n"), removal_info.format(true));
        for (auto & user_id : key_info.get_user_ids()) {
            std::cerr << libdnf5::utils::sformat(_(" UserID     : \"{}\"\n"), user_id);
        }
        std::cerr << libdnf5::utils::sformat(_(" Fingerprint: {}\n"), key_info.get_fingerprint());

        std::cerr << std::endl << _("As a result, installing packages signed with this key will fail.") << std::endl;
        std::cerr << _("It is recommended to remove the expired key to allow importing") << std::endl;
        std::cerr << _("an updated key. This might leave already installed packages unverifiable.") << std::endl
                  << std::endl;

        std::cerr << _("The system will now proceed with removing the key.") << std::endl;

        return libdnf5::cli::utils::userconfirm::userconfirm(*config);
    }

    void repokey_removed([[maybe_unused]] const libdnf5::rpm::KeyInfo & key_info) override {
        std::cerr << libdnf5::utils::sformat(_("Key 0x{} was successfully removed."), key_info.get_short_key_id())
                  << std::endl;
    }

private:
    libdnf5::ConfigMain * config;
};

}  // namespace

class Context::Impl {
public:
    explicit Impl(Context & owner, std::vector<std::unique_ptr<libdnf5::Logger>> && loggers)
        : owner(owner),
          base(std::move(loggers)),
          plugins(std::make_unique<Plugins>(owner)) {}

    void apply_repository_setopts();

    void update_repo_metadata_from_specs(const std::vector<std::string> & pkg_specs);

    void update_repo_metadata_from_advisory_options(
        const std::vector<std::string> & names,
        bool security,
        bool bugfix,
        bool enhancement,
        bool newpackage,
        const std::vector<std::string> & severity,
        const std::vector<std::string> & bzs,
        const std::vector<std::string> & cves);

    void load_repos(bool load_system);

    void store_offline(libdnf5::base::Transaction & transaction);

    const char * get_comment() const noexcept { return comment; }

    void set_comment(const char * comment) noexcept { this->comment = comment; }

    std::string get_cmdline() { return cmdline; }

    void set_cmdline(std::string & cmdline) { this->cmdline = cmdline; }

    void download_and_run(libdnf5::base::Transaction & transaction);

    void set_quiet(bool quiet) { this->quiet = quiet; }

    bool get_quiet() const { return quiet; }

    void set_dump_main_config(bool enable) { this->dump_main_config = enable; }

    bool get_dump_main_config() const { return dump_main_config; }

    void set_dump_repo_config_id_list(const std::vector<std::string> & repo_id_list) {
        this->dump_repo_config_id_list = repo_id_list;
    }

    const std::vector<std::string> & get_dump_repo_config_id_list() const { return dump_repo_config_id_list; }

    void set_dump_variables(bool enable) { this->dump_variables = enable; }

    bool get_dump_variables() const { return dump_variables; }

    void set_show_new_leaves(bool show_new_leaves) { this->show_new_leaves = show_new_leaves; }

    bool get_show_new_leaves() const { return show_new_leaves; }

    Plugins & get_plugins() { return *plugins; }

    libdnf5::Goal * get_goal(bool new_if_not_exist);

    void set_transaction(libdnf5::base::Transaction && transaction) {
        this->transaction = std::make_unique<libdnf5::base::Transaction>(std::move(transaction));
    }

    libdnf5::base::Transaction * get_transaction() { return transaction.get(); }

    void set_load_system_repo(bool on) { load_system_repo = on; }
    bool get_load_system_repo() const noexcept { return load_system_repo; }

    void set_load_available_repos(LoadAvailableRepos which) { load_available_repos = which; }
    LoadAvailableRepos get_load_available_repos() const noexcept { return load_available_repos; }

    void print_output(std::string_view msg) const;
    void print_info(std::string_view msg) const;
    void print_error(std::string_view msg) const;

    void set_output_stream(std::ostream & new_output_stream) { out_stream = new_output_stream; }
    void set_error_stream(std::ostream & new_error_stream) { err_stream = new_error_stream; }

    void set_transaction_store_path(std::filesystem::path path) { transaction_store_path = path; }
    const std::filesystem::path & get_transaction_store_path() const { return transaction_store_path; }

    void set_should_store_offline(bool should_store_offline) { this->should_store_offline = should_store_offline; }
    bool get_should_store_offline() const { return should_store_offline; }

    void set_json_output_requested(bool json_output) {
        this->json_output = json_output;
        if (json_output) {
            set_quiet(true);
        }
    }
    bool get_json_output_requested() const { return json_output; }

    void set_create_repos(bool create_repos) { this->create_repos = create_repos; }
    bool get_create_repos() const { return create_repos; }

    libdnf5::Base & get_base() { return base; };

    std::vector<std::pair<std::string, std::string>> & get_setopts() { return setopts; }
    const std::vector<std::pair<std::string, std::string>> & get_setopts() const { return setopts; }

    std::vector<std::pair<std::string, std::string>> & get_repos_from_path() { return repos_from_path; }
    const std::vector<std::pair<std::string, std::string>> & get_repos_from_path() const { return repos_from_path; }

    std::vector<std::pair<std::vector<std::string>, bool>> & get_libdnf_plugins_enablement() {
        return libdnf_plugins_enablement;
    }
    const std::vector<std::pair<std::vector<std::string>, bool>> & get_libdnf_plugins_enablement() const {
        return libdnf_plugins_enablement;
    }

private:
    Context & owner;

    std::filesystem::path transaction_store_path;

    libdnf5::Base base;
    std::vector<std::pair<std::string, std::string>> setopts;
    std::vector<std::pair<std::string, std::string>> repos_from_path;

    /// list of lists of libdnf plugin names (global patterns) that we want to enable (true) or disable (false)
    std::vector<std::pair<std::vector<std::string>, bool>> libdnf_plugins_enablement;

    std::string cmdline;

    /// Points to user comment.
    const char * comment{nullptr};

    bool should_store_offline = false;
    bool json_output = false;
    bool create_repos = true;

    bool quiet{false};
    bool dump_main_config{false};
    std::vector<std::string> dump_repo_config_id_list;
    bool dump_variables{false};
    bool show_new_leaves{false};
    std::string get_cmd_line();

    std::reference_wrapper<std::ostream> out_stream = std::cout;
    std::reference_wrapper<std::ostream> err_stream = std::cerr;

    std::unique_ptr<Plugins> plugins;
    std::unique_ptr<libdnf5::Goal> goal;
    std::unique_ptr<libdnf5::base::Transaction> transaction;

    bool load_system_repo{false};
    LoadAvailableRepos load_available_repos{LoadAvailableRepos::NONE};
};

// TODO(jrohel): Move logic into libdnf?
void Context::Impl::apply_repository_setopts() {
    std::vector<std::string> missing_repo_ids;
    for (const auto & setopt : setopts) {
        auto last_dot_pos = setopt.first.rfind('.');
        auto repo_pattern = setopt.first.substr(0, last_dot_pos);
        libdnf5::repo::RepoQuery query(base);
        query.filter_id(repo_pattern, libdnf5::sack::QueryCmp::GLOB);
        query.filter_type(libdnf5::repo::Repo::Type::AVAILABLE);
        if (query.empty()) {
            missing_repo_ids.push_back(repo_pattern);
        }
        auto key = setopt.first.substr(last_dot_pos + 1);
        for (auto & repo : query) {
            try {
                repo->get_config().opt_binds().at(key).new_string(
                    libdnf5::Option::Priority::COMMANDLINE, setopt.second);
            } catch (const std::exception & ex) {
                print_error(
                    libdnf5::utils::sformat(_("setopt: \"{}.{}\": {}"), setopt.first, setopt.second, ex.what()));
            }
        }
    }

    if (!missing_repo_ids.empty()) {
        auto missing_repo_ids_string = libdnf5::utils::string::join(missing_repo_ids, _(", "));
        if (base.get_config().get_installroot_option().get_value() != "/" &&
            !base.get_config().get_use_host_config_option().get_value()) {
            throw libdnf5::cli::ArgumentParserError(
                M_("No matching repositories for {}. To use repositories from a host system, pass --use-host-config "
                   "option"),
                missing_repo_ids_string);
        } else {
            throw libdnf5::cli::ArgumentParserError(M_("No matching repositories for {}"), missing_repo_ids_string);
        }
    }
}

void Context::Impl::update_repo_metadata_from_specs(const std::vector<std::string> & pkg_specs) {
    for (auto & spec : pkg_specs) {
        if (libdnf5::utils::is_file_pattern(spec) && !spec.ends_with(".rpm")) {
            base.get_config().get_optional_metadata_types_option().add_item(
                libdnf5::Option::Priority::RUNTIME, libdnf5::METADATA_TYPE_FILELISTS);
            return;
        }
        if (spec.starts_with('@')) {
            base.get_config().get_optional_metadata_types_option().add_item(
                libdnf5::Option::Priority::RUNTIME, libdnf5::METADATA_TYPE_COMPS);
            return;
        }
    }
}

void Context::Impl::update_repo_metadata_from_advisory_options(
    const std::vector<std::string> & names,
    bool security,
    bool bugfix,
    bool enhancement,
    bool newpackage,
    const std::vector<std::string> & severity,
    const std::vector<std::string> & bzs,
    const std::vector<std::string> & cves) {
    bool updateinfo_needed = !names.empty() || security || bugfix || enhancement || newpackage || !severity.empty() ||
                             !bzs.empty() || !cves.empty();
    if (updateinfo_needed) {
        base.get_config().get_optional_metadata_types_option().add_item(
            libdnf5::Option::Priority::RUNTIME, libdnf5::METADATA_TYPE_UPDATEINFO);
    }
}

void Context::Impl::load_repos(bool load_system) {
    libdnf5::repo::RepoQuery repos(base);
    repos.filter_enabled(true);
    repos.filter_type(libdnf5::repo::Repo::Type::SYSTEM, libdnf5::sack::QueryCmp::NEQ);

    for (auto & repo : repos) {
        repo->set_callbacks(std::make_unique<dnf5::KeyImportRepoCB>(base.get_config()));
    }

    print_info(_("Updating and loading repositories:"));
    if (load_system) {
        base.get_repo_sack()->load_repos();
    } else {
        base.get_repo_sack()->load_repos(libdnf5::repo::Repo::Type::AVAILABLE);
    }

    if (auto download_callbacks = dynamic_cast<DownloadCallbacks *>(base.get_download_callbacks())) {
        download_callbacks->reset_progress_bar();
    }
    print_info(_("Repositories loaded."));
}

void Context::Impl::store_offline(libdnf5::base::Transaction & transaction) {
    // Test the transaction
    base.get_config().get_tsflags_option().set(libdnf5::Option::Priority::RUNTIME, "test");
    print_info(_("Testing offline transaction"));
    auto result = transaction.run();
    if (result != libdnf5::base::Transaction::TransactionRunResult::SUCCESS) {
        print_error(libdnf5::utils::sformat(
            _("Transaction failed: {}"), libdnf5::base::Transaction::transaction_result_to_string(result)));
        for (auto const & entry : transaction.get_gpg_signature_problems()) {
            print_error(entry);
        }
        for (auto & problem : transaction.get_transaction_problems()) {
            print_error(libdnf5::utils::sformat(_("  - {}"), problem));
        }
        throw libdnf5::cli::SilentCommandExitError(1);
    }

    for (auto const & entry : transaction.get_gpg_signature_problems()) {
        print_error(entry);
    }

    // Serialize the transaction
    const auto & installroot = base.get_config().get_installroot_option().get_value();
    const auto & offline_datadir = installroot / libdnf5::offline::DEFAULT_DATADIR.relative_path();
    std::filesystem::create_directories(offline_datadir);
    constexpr const char * packages_in_trans_dir{"./packages"};
    constexpr const char * comps_in_trans_dir{"./comps"};
    const auto & comps_location = offline_datadir / comps_in_trans_dir;

    const std::filesystem::path state_path{offline_datadir / libdnf5::offline::TRANSACTION_STATE_FILENAME};
    libdnf5::offline::OfflineTransactionState state{state_path};

    auto & offline_data = state.get_data();
    offline_data.set_status(libdnf5::offline::STATUS_DOWNLOAD_INCOMPLETE);
    state.write();

    transaction.store_comps(comps_location);

    const auto transaction_json_path = offline_datadir / TRANSACTION_JSON;
    libdnf5::utils::fs::File transaction_json_file{transaction_json_path, "w"};
    transaction_json_file.write(transaction.serialize(packages_in_trans_dir, comps_in_trans_dir));
    transaction_json_file.close();

    // Download and transaction test complete. Fill out entries in offline
    // transaction state file.
    offline_data.set_status(libdnf5::offline::STATUS_DOWNLOAD_COMPLETE);
    offline_data.set_cachedir(base.get_config().get_cachedir_option().get_value());

    std::vector<std::string> command_vector;
    auto * current_command = owner.get_selected_command();
    while (current_command != owner.get_root_command()) {
        command_vector.insert(command_vector.begin(), current_command->get_argument_parser_command()->get_id());
        current_command = current_command->get_parent_command();
    }
    offline_data.set_verb(libdnf5::utils::string::join(command_vector, " "));
    offline_data.set_cmd_line(get_cmdline());

    const auto & detected_releasever = libdnf5::Vars::detect_release(base.get_weak_ptr(), installroot);
    if (detected_releasever != nullptr) {
        offline_data.set_system_releasever(*detected_releasever);
    }
    offline_data.set_target_releasever(base.get_vars()->get_value("releasever"));

    if (!base.get_config().get_module_platform_id_option().empty()) {
        offline_data.set_module_platform_id(base.get_config().get_module_platform_id_option().get_value());
    }

    state.write();
}

void Context::Impl::download_and_run(libdnf5::base::Transaction & transaction) {
    if (!transaction_store_path.empty()) {
        auto transaction_location = transaction_store_path / TRANSACTION_JSON;
        constexpr const char * packages_in_trans_dir{"./packages"};
        auto packages_location = transaction_store_path / packages_in_trans_dir;
        constexpr const char * comps_in_trans_dir{"./comps"};
        auto comps_location = transaction_store_path / comps_in_trans_dir;
        if (std::filesystem::exists(transaction_location)) {
            print_error(libdnf5::utils::sformat(
                _("Location \"{}\" already contains a stored transaction, it will be overwritten."),
                transaction_store_path.string()));
            if (libdnf5::cli::utils::userconfirm::userconfirm(base.get_config())) {
                std::filesystem::remove_all(packages_location);
                std::filesystem::remove_all(comps_location);
            } else {
                throw libdnf5::cli::AbortedByUserError();
            }
        }
        auto & destdir_opt = base.get_config().get_destdir_option();
        destdir_opt.set(packages_location);
        std::filesystem::create_directories(transaction_store_path);
        transaction.download();
        transaction.store_comps(comps_location);
        libdnf5::utils::fs::File transfile(transaction_location, "w");
        transfile.write(transaction.serialize(packages_in_trans_dir, comps_in_trans_dir));
        return;
    }

    if (should_store_offline) {
        const auto & installroot = base.get_config().get_installroot_option().get_value();
        const auto & offline_datadir = installroot / libdnf5::offline::DEFAULT_DATADIR.relative_path();
        std::filesystem::create_directories(offline_datadir);
        const std::filesystem::path state_path{offline_datadir / libdnf5::offline::TRANSACTION_STATE_FILENAME};
        libdnf5::offline::OfflineTransactionState state{state_path};

        // Check whether there is another pending offline transaction present
        auto & offline_data = state.get_data();
        if (offline_data.get_status() != libdnf5::offline::STATUS_DOWNLOAD_INCOMPLETE) {
            print_error(_("There is already an offline transaction queued, initiated by the following command:"));
            print_error(fmt::format("\t{}", offline_data.get_cmd_line()));
            print_error(_("Continuing will cancel the old offline transaction and replace it with this one."));
            if (!libdnf5::cli::utils::userconfirm::userconfirm(base.get_config())) {
                throw libdnf5::cli::AbortedByUserError();
            }
        }

        base.get_config().get_destdir_option().set(offline_datadir / "packages");
        transaction.set_download_local_pkgs(true);
    }

    transaction.download();
    if (base.get_config().get_downloadonly_option().get_value()) {
        return;
    }

    if (should_store_offline) {
        store_offline(transaction);
        print_info(
            _("Transaction stored to be performed offline. Run `dnf5 offline reboot` to reboot and run the "
              "transaction. To cancel the transaction and delete the downloaded files, use `dnf5 "
              "offline clean`."));
        return;
    }

    print_info(_("Running transaction"));

    // Compute the total number of transaction actions (number of bars)
    // Total number of actions = number of packages in the transaction +
    //                           action of preparing transaction
    const auto & trans_packages = transaction.get_transaction_packages();
    auto num_of_actions = trans_packages.size() + 1;

    auto callbacks = std::make_unique<RpmTransCB>(owner);
    callbacks->get_multi_progress_bar()->set_total_num_of_bars(num_of_actions);
    transaction.set_callbacks(std::move(callbacks));

    transaction.set_description(get_cmdline());

    if (comment) {
        transaction.set_comment(comment);
    }

    auto result = transaction.run();
    if (result != libdnf5::base::Transaction::TransactionRunResult::SUCCESS) {
        print_error(libdnf5::utils::sformat(
            _("Transaction failed: {}"), libdnf5::base::Transaction::transaction_result_to_string(result)));
        for (auto const & entry : transaction.get_gpg_signature_problems()) {
            print_error(entry);
        }
        for (auto & problem : transaction.get_transaction_problems()) {
            print_error(libdnf5::utils::sformat(_("  - {}"), problem));
        }
        throw libdnf5::cli::SilentCommandExitError(1);
    }

    for (auto const & entry : transaction.get_gpg_signature_problems()) {
        print_error(entry);
    }
    // TODO(mblaha): print a summary of successful transaction
}

libdnf5::Goal * Context::Impl::get_goal(bool new_if_not_exist) {
    if (!goal && new_if_not_exist) {
        goal = std::make_unique<libdnf5::Goal>(base);
    }
    return goal.get();
}

void Context::Impl::print_output(std::string_view msg) const {
    out_stream.get() << msg << std::endl;
}
void Context::Impl::print_info(std::string_view msg) const {
    if (!quiet) {
        err_stream.get() << msg << std::endl;
    }
}
void Context::Impl::print_error(std::string_view msg) const {
    err_stream.get() << msg << std::endl;
}


Context::Context(std::vector<std::unique_ptr<libdnf5::Logger>> && loggers)
    : p_impl{new Impl(*this, std::move(loggers))} {}

Context::~Context() {
    // "Session", which is the parent of "Context", owns objects from dnf5 plugins (command arguments).
    // Objects from plugins must be destroyed before the plugins can be released,
    // otherwise they will reference the released code.
    // TODO(jrohel): Calling clear() is not nice here. Better workflow.
    clear();
}

void Context::apply_repository_setopts() {
    p_impl->apply_repository_setopts();
}

void Context::update_repo_metadata_from_specs(const std::vector<std::string> & pkg_specs) {
    p_impl->update_repo_metadata_from_specs(pkg_specs);
}

void Context::update_repo_metadata_from_advisory_options(
    const std::vector<std::string> & names,
    bool security,
    bool bugfix,
    bool enhancement,
    bool newpackage,
    const std::vector<std::string> & severity,
    const std::vector<std::string> & bzs,
    const std::vector<std::string> & cves) {
    p_impl->update_repo_metadata_from_advisory_options(
        names, security, bugfix, enhancement, newpackage, severity, bzs, cves);
}

void Context::load_repos(bool load_system) {
    p_impl->load_repos(load_system);
}

void Context::store_offline(libdnf5::base::Transaction & transaction) {
    p_impl->store_offline(transaction);
}

const char * Context::get_comment() const noexcept {
    return p_impl->get_comment();
}

void Context::set_comment(const char * comment) noexcept {
    p_impl->set_comment(comment);
}

std::string Context::get_cmdline() {
    return p_impl->get_cmdline();
}

void Context::set_cmdline(std::string & cmdline) {
    p_impl->set_cmdline(cmdline);
}

void Context::download_and_run(libdnf5::base::Transaction & transaction) {
    p_impl->download_and_run(transaction);
}

void Context::set_quiet(bool quiet) {
    p_impl->set_quiet(quiet);
}

bool Context::get_quiet() const {
    return p_impl->get_quiet();
}

void Context::set_dump_main_config(bool enable) {
    p_impl->set_dump_main_config(enable);
}

bool Context::get_dump_main_config() const {
    return p_impl->get_dump_main_config();
}

void Context::set_dump_repo_config_id_list(const std::vector<std::string> & repo_id_list) {
    p_impl->set_dump_repo_config_id_list(repo_id_list);
}

const std::vector<std::string> & Context::get_dump_repo_config_id_list() const {
    return p_impl->get_dump_repo_config_id_list();
}

void Context::set_dump_variables(bool enable) {
    p_impl->set_dump_variables(enable);
}

bool Context::get_dump_variables() const {
    return p_impl->get_dump_variables();
}

void Context::set_show_new_leaves(bool show_new_leaves) {
    p_impl->set_show_new_leaves(show_new_leaves);
}

bool Context::get_show_new_leaves() const {
    return p_impl->get_show_new_leaves();
}

Plugins & Context::get_plugins() {
    return p_impl->get_plugins();
}

libdnf5::Goal * Context::get_goal(bool new_if_not_exist) {
    return p_impl->get_goal(new_if_not_exist);
}

void Context::set_transaction(libdnf5::base::Transaction && transaction) {
    p_impl->set_transaction(std::move(transaction));
}

libdnf5::base::Transaction * Context::get_transaction() {
    return p_impl->get_transaction();
}

void Context::set_load_system_repo(bool on) {
    p_impl->set_load_system_repo(on);
}
bool Context::get_load_system_repo() const noexcept {
    return p_impl->get_load_system_repo();
}

void Context::set_load_available_repos(LoadAvailableRepos which) {
    p_impl->set_load_available_repos(which);
}

Context::LoadAvailableRepos Context::get_load_available_repos() const noexcept {
    return p_impl->get_load_available_repos();
}

void Context::print_output(std::string_view msg) const {
    p_impl->print_output(msg);
}
void Context::print_info(std::string_view msg) const {
    p_impl->print_info(msg);
}
void Context::print_error(std::string_view msg) const {
    p_impl->print_error(msg);
}
void Context::set_output_stream(std::ostream & new_output_stream) {
    p_impl->set_output_stream(new_output_stream);
}
void Context::set_error_stream(std::ostream & new_error_stream) {
    p_impl->set_error_stream(new_error_stream);
}

void Context::set_transaction_store_path(std::filesystem::path path) {
    p_impl->set_transaction_store_path(path);
}

const std::filesystem::path & Context::get_transaction_store_path() const {
    return p_impl->get_transaction_store_path();
}

void Context::set_should_store_offline(bool should_store_offline) {
    p_impl->set_should_store_offline(should_store_offline);
}

bool Context::get_should_store_offline() const {
    return p_impl->get_should_store_offline();
}

void Context::set_json_output_requested(bool json_output) {
    p_impl->set_json_output_requested(json_output);
}

bool Context::get_json_output_requested() const {
    return p_impl->get_json_output_requested();
}

void Context::set_create_repos(bool create_repos) {
    p_impl->set_create_repos(create_repos);
}

bool Context::get_create_repos() const {
    return p_impl->get_create_repos();
}

libdnf5::Base & Context::get_base() {
    return p_impl->get_base();
};

std::vector<std::pair<std::string, std::string>> & Context::get_setopts() {
    return p_impl->get_setopts();
}

const std::vector<std::pair<std::string, std::string>> & Context::get_setopts() const {
    return p_impl->get_setopts();
}

std::vector<std::pair<std::string, std::string>> & Context::get_repos_from_path() {
    return p_impl->get_repos_from_path();
}

const std::vector<std::pair<std::string, std::string>> & Context::get_repos_from_path() const {
    return p_impl->get_repos_from_path();
}

std::vector<std::pair<std::vector<std::string>, bool>> & Context::get_libdnf_plugins_enablement() {
    return p_impl->get_libdnf_plugins_enablement();
}

const std::vector<std::pair<std::vector<std::string>, bool>> & Context::get_libdnf_plugins_enablement() const {
    return p_impl->get_libdnf_plugins_enablement();
}


Command::~Command() = default;

Context & Command::get_context() const noexcept {
    return static_cast<Context &>(get_session());
}

void Command::goal_resolved() {
    auto & transaction = *get_context().get_transaction();
    if (transaction.get_problems() != libdnf5::GoalProblem::NO_PROBLEM) {
        throw libdnf5::cli::GoalResolveError(transaction);
    }
}


RpmTransCB::RpmTransCB(Context & context) : context(context) {
    multi_progress_bar.set_total_bar_visible_limit(libdnf5::cli::progressbar::MultiProgressBar::NEVER_VISIBLE_LIMIT);
}

RpmTransCB::~RpmTransCB() {
    if (active_progress_bar && active_progress_bar->get_state() != libdnf5::cli::progressbar::ProgressBarState::ERROR) {
        active_progress_bar->set_state(libdnf5::cli::progressbar::ProgressBarState::SUCCESS);
    }
    if (active_progress_bar) {
        multi_progress_bar.print();
    }
}

libdnf5::cli::progressbar::MultiProgressBar * RpmTransCB::get_multi_progress_bar() {
    return &multi_progress_bar;
}

int RpmTransCB::rpm_messages_to_progress(const libdnf5::cli::progressbar::MessageType message_type) {
    auto transaction = context.get_transaction();
    int retval = 0;
    for (const auto & msg : transaction->get_rpm_messages()) {
        active_progress_bar->add_message(message_type, libdnf5::utils::sformat("[RPM] {}", msg));
        ++retval;
    }
    return retval;
}

void RpmTransCB::install_progress(
    [[maybe_unused]] const libdnf5::base::TransactionPackage & item, uint64_t amount, [[maybe_unused]] uint64_t total) {
    active_progress_bar->set_ticks(static_cast<int64_t>(amount));
    if (is_time_to_print()) {
        multi_progress_bar.print();
    }
}

void RpmTransCB::install_start(const libdnf5::base::TransactionPackage & item, uint64_t total) {
    const char * msg{nullptr};
    switch (item.get_action()) {
        case libdnf5::transaction::TransactionItemAction::UPGRADE:
            msg = _("Upgrading {}");
            break;
        case libdnf5::transaction::TransactionItemAction::DOWNGRADE:
            msg = _("Downgrading {}");
            break;
        case libdnf5::transaction::TransactionItemAction::REINSTALL:
            msg = _("Reinstalling {}");
            break;
        case libdnf5::transaction::TransactionItemAction::INSTALL:
        case libdnf5::transaction::TransactionItemAction::REMOVE:
        case libdnf5::transaction::TransactionItemAction::REPLACED:
            break;
        case libdnf5::transaction::TransactionItemAction::REASON_CHANGE:
        case libdnf5::transaction::TransactionItemAction::ENABLE:
        case libdnf5::transaction::TransactionItemAction::DISABLE:
        case libdnf5::transaction::TransactionItemAction::RESET:
        case libdnf5::transaction::TransactionItemAction::SWITCH:
            auto & logger = *context.get_base().get_logger();
            logger.warning(
                _("Unexpected action in TransactionPackage: {}"),
                static_cast<std::underlying_type_t<libdnf5::base::Transaction::TransactionRunResult>>(
                    item.get_action()));
            return;
    }
    if (!msg) {
        msg = _("Installing {}");
    }
    new_progress_bar(static_cast<int64_t>(total), libdnf5::utils::sformat(msg, item.get_package().get_full_nevra()));
}

void RpmTransCB::install_stop(
    [[maybe_unused]] const libdnf5::base::TransactionPackage & item,
    [[maybe_unused]] uint64_t amount,
    [[maybe_unused]] uint64_t total) {
    rpm_messages_to_progress(libdnf5::cli::progressbar::MessageType::WARNING);
    multi_progress_bar.print();
}

void RpmTransCB::transaction_progress(uint64_t amount, [[maybe_unused]] uint64_t total) {
    active_progress_bar->set_ticks(static_cast<int64_t>(amount));
    if (is_time_to_print()) {
        multi_progress_bar.print();
    }
}

void RpmTransCB::transaction_start(uint64_t total) {
    new_progress_bar(static_cast<int64_t>(total), _("Prepare transaction"));
}

void RpmTransCB::transaction_stop([[maybe_unused]] uint64_t total) {
    active_progress_bar->set_ticks(static_cast<int64_t>(total));
    multi_progress_bar.print();
}

void RpmTransCB::uninstall_progress(
    [[maybe_unused]] const libdnf5::base::TransactionPackage & item, uint64_t amount, [[maybe_unused]] uint64_t total) {
    active_progress_bar->set_ticks(static_cast<int64_t>(amount));
    if (is_time_to_print()) {
        multi_progress_bar.print();
    }
}

void RpmTransCB::uninstall_start(const libdnf5::base::TransactionPackage & item, uint64_t total) {
    const char * msg{nullptr};
    if (item.get_action() == libdnf5::transaction::TransactionItemAction::REMOVE ||
        item.get_action() == libdnf5::transaction::TransactionItemAction::REPLACED) {
        msg = _("Removing {}");
    }
    if (!msg) {
        msg = _("Cleanup {}");
    }
    new_progress_bar(static_cast<int64_t>(total), libdnf5::utils::sformat(msg, item.get_package().get_full_nevra()));
}

void RpmTransCB::uninstall_stop(
    [[maybe_unused]] const libdnf5::base::TransactionPackage & item,
    [[maybe_unused]] uint64_t amount,
    [[maybe_unused]] uint64_t total) {
    rpm_messages_to_progress(libdnf5::cli::progressbar::MessageType::WARNING);
    multi_progress_bar.print();
}


void RpmTransCB::unpack_error(const libdnf5::base::TransactionPackage & item) {
    rpm_messages_to_progress(libdnf5::cli::progressbar::MessageType::ERROR);
    active_progress_bar->add_message(
        libdnf5::cli::progressbar::MessageType::ERROR,
        libdnf5::utils::sformat(_("Unpack error: {}"), item.get_package().get_full_nevra()));
    active_progress_bar->set_state(libdnf5::cli::progressbar::ProgressBarState::ERROR);
    multi_progress_bar.print();
}

void RpmTransCB::cpio_error(const libdnf5::base::TransactionPackage & item) {
    active_progress_bar->add_message(
        libdnf5::cli::progressbar::MessageType::ERROR,
        libdnf5::utils::sformat(_("Cpio error: {}"), item.get_package().get_full_nevra()));
    active_progress_bar->set_state(libdnf5::cli::progressbar::ProgressBarState::ERROR);
    multi_progress_bar.print();
}

int RpmTransCB::script_output_to_progress(const libdnf5::cli::progressbar::MessageType message_type) {
    auto transaction = context.get_transaction();
    auto output = transaction->get_last_script_output();
    int retval = 0;
    if (!output.empty()) {
        active_progress_bar->add_message(message_type, _("Scriptlet output:"));
        for (auto & line : libdnf5::utils::string::split(output, "\n")) {
            active_progress_bar->add_message(message_type, line);
            ++retval;
        }
    }
    return retval;
}

void RpmTransCB::script_error(
    [[maybe_unused]] const libdnf5::base::TransactionPackage * item,
    [[maybe_unused]] libdnf5::rpm::Nevra nevra,
    [[maybe_unused]] libdnf5::rpm::TransactionCallbacks::ScriptType type,
    [[maybe_unused]] uint64_t return_code) {}

void RpmTransCB::script_start(
    [[maybe_unused]] const libdnf5::base::TransactionPackage * item,
    libdnf5::rpm::Nevra nevra,
    libdnf5::rpm::TransactionCallbacks::ScriptType type) {
    if (!active_progress_bar) {
        // Scripts could potentially be the first thing in a transaction if the verification stage
        // is skipped, so create a progress bar for the scripts to use if one doesn't already exist.
        multi_progress_bar.set_total_num_of_bars(multi_progress_bar.get_total_num_of_bars() + 1);
        new_progress_bar(static_cast<int64_t>(-1), _("Running scriptlets"));
    }
    active_progress_bar->add_message(
        libdnf5::cli::progressbar::MessageType::INFO,
        libdnf5::utils::sformat(
            _("Running {} scriptlet: {}"), script_type_to_string(type), to_full_nevra_string(nevra)));
    multi_progress_bar.print();
}

void RpmTransCB::script_stop(
    [[maybe_unused]] const libdnf5::base::TransactionPackage * item,
    libdnf5::rpm::Nevra nevra,
    libdnf5::rpm::TransactionCallbacks::ScriptType type,
    uint64_t return_code) {
    libdnf5::cli::progressbar::MessageType message_type = libdnf5::cli::progressbar::MessageType::WARNING;
    switch (return_code) {
        case RPMRC_OK:
            active_progress_bar->add_message(
                libdnf5::cli::progressbar::MessageType::INFO,
                libdnf5::utils::sformat(
                    _("Finished {} scriptlet: {}"), script_type_to_string(type), to_full_nevra_string(nevra)));
            break;
        case RPMRC_NOTFOUND:
            active_progress_bar->add_message(
                libdnf5::cli::progressbar::MessageType::WARNING,
                libdnf5::utils::sformat(
                    _("Non-critical error in {} scriptlet: {}"),
                    script_type_to_string(type),
                    to_full_nevra_string(nevra)));
            break;
        default:
            message_type = libdnf5::cli::progressbar::MessageType::ERROR;
            active_progress_bar->add_message(
                libdnf5::cli::progressbar::MessageType::ERROR,
                libdnf5::utils::sformat(
                    _("Error in {} scriptlet: {}"), script_type_to_string(type), to_full_nevra_string(nevra)));
            break;
    }
    int loglines = script_output_to_progress(message_type);
    loglines += rpm_messages_to_progress(message_type);
    if (return_code == RPMRC_OK && loglines == 0) {
        // remove the script start/stop messages in case the script
        // finished successfully and no rpm log message or scriptlet
        // output was printed.
        active_progress_bar->pop_message();
        active_progress_bar->pop_message();
    }
    multi_progress_bar.print();
}

void RpmTransCB::elem_progress(
    [[maybe_unused]] const libdnf5::base::TransactionPackage & item,
    [[maybe_unused]] uint64_t amount,
    [[maybe_unused]] uint64_t total) {
    //std::cout << libdnf5::utils::sformat(_("Element progress: {} {}/{}"), item.get_package().get_full_nevra(), amount, total) << std::endl;
}

void RpmTransCB::verify_progress(uint64_t amount, [[maybe_unused]] uint64_t total) {
    active_progress_bar->set_ticks(static_cast<int64_t>(amount));
    if (is_time_to_print()) {
        multi_progress_bar.print();
    }
}

void RpmTransCB::verify_start([[maybe_unused]] uint64_t total) {
    // Verification of new packages entering the system is the first step. However, this step may not be performed
    // if the transaction does not contain new packages or if verification is disabled.
    // If verification is performed, we increase the total number of progress bars in multi_progress_bar and
    // create a progress bar for verification.
    multi_progress_bar.set_total_num_of_bars(multi_progress_bar.get_total_num_of_bars() + 1);
    new_progress_bar(static_cast<int64_t>(total), _("Verify package files"));
}

void RpmTransCB::verify_stop([[maybe_unused]] uint64_t total) {
    active_progress_bar->set_ticks(static_cast<int64_t>(total));
    multi_progress_bar.print();
}

void RpmTransCB::new_progress_bar(int64_t total, const std::string & descr) {
    if (active_progress_bar && active_progress_bar->get_state() != libdnf5::cli::progressbar::ProgressBarState::ERROR) {
        active_progress_bar->set_state(libdnf5::cli::progressbar::ProgressBarState::SUCCESS);
    }
    auto progress_bar =
        std::make_unique<libdnf5::cli::progressbar::DownloadProgressBar>(static_cast<int64_t>(total), descr);
    progress_bar->set_auto_finish(false);
    progress_bar->start();
    active_progress_bar = progress_bar.get();
    multi_progress_bar.add_bar(std::move(progress_bar));
}

bool RpmTransCB::is_time_to_print() {
    auto now = std::chrono::steady_clock::now();
    auto delta = now - prev_print_time;
    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(delta).count();
    if (ms > 100) {
        // 100ms equals to 10 FPS and that seems to be smooth enough
        prev_print_time = now;
        return true;
    }
    return false;
}

std::chrono::time_point<std::chrono::steady_clock> RpmTransCB::prev_print_time = std::chrono::steady_clock::now();


/// Returns file and directory paths that begins with `path_to_complete`.
/// Files must match `regex_pattern`.
static std::pair<std::vector<std::string>, std::vector<std::string>> complete_paths(
    const std::string & path_to_complete, const std::regex & regex_pattern) {
    std::pair<std::vector<std::string>, std::vector<std::string>> ret;

    const fs::path ppath_to_complete(path_to_complete);
    fs::path parent_path = ppath_to_complete.parent_path();
    if (parent_path.empty()) {
        parent_path = ".";
    }

    const bool path_to_complete_prefix_dot_slash = path_to_complete[0] == '.' && path_to_complete[1] == '/';
    const bool filename_to_complete_starts_with_dot = ppath_to_complete.filename().native()[0] == '.';
    std::error_code ec;  // Do not report errors when constructing a directory iterator
    for (const auto & dir_entry : fs::directory_iterator(parent_path, ec)) {
        const auto filename = dir_entry.path().filename();

        // Skips hidden entries (starting with a dot) unless explicitly requested by `path_to_complete`.
        if (!filename_to_complete_starts_with_dot && filename.native()[0] == '.') {
            continue;
        }

        std::string dir_entry_path;
        const auto & raw_dir_entry_path = dir_entry.path().native();
        if (path_to_complete_prefix_dot_slash) {
            dir_entry_path = raw_dir_entry_path;
        } else {
            dir_entry_path = raw_dir_entry_path[0] == '.' && raw_dir_entry_path[1] == '/'
                                 ? raw_dir_entry_path.substr(2)  // remove "./" prefix
                                 : raw_dir_entry_path;
        }

        if (dir_entry_path.compare(0, path_to_complete.length(), path_to_complete) == 0) {
            // Adds the directory.
            // Only directories that contain files that match the pattern or contain subdirectories are added.
            if (dir_entry.is_directory()) {
                bool complete = false;
                for (const auto & subdir_entry : fs::directory_iterator(dir_entry.path(), ec)) {
                    if ((subdir_entry.is_regular_file() &&
                         std::regex_match(subdir_entry.path().filename().native(), regex_pattern)) ||
                        subdir_entry.is_directory()) {
                        complete = true;
                        break;
                    }
                }
                if (complete) {
                    ret.second.push_back(dir_entry_path + '/');
                }
                continue;
            }

            // Adds the file if it matches the pattern.
            if (dir_entry.is_regular_file() && std::regex_match(filename.native(), regex_pattern)) {
                ret.first.push_back(dir_entry_path);
            }
        }
    }

    return ret;
}

std::vector<std::string> match_specs(
    Context & ctx,
    const std::string & pattern,
    bool installed,
    bool available,
    bool paths,
    bool only_nevras,
    const char * file_name_regex) {
    auto & base = ctx.get_base();

    base.get_config().get_assumeno_option().set(libdnf5::Option::Priority::RUNTIME, true);
    ctx.set_quiet(true);

    base.load_config();
    base.setup();

    // optimization - disable the search for matching installed and available packages for file patterns
    if (libdnf5::utils::is_file_pattern(pattern)) {
        installed = available = false;
    }

    if (available) {
        try {
            // create rpm repositories according configuration files
            base.get_repo_sack()->create_repos_from_system_configuration();
            base.get_config().get_optional_metadata_types_option().set(
                libdnf5::Option::Priority::RUNTIME, libdnf5::OptionStringSet::ValueType{});

            ctx.apply_repository_setopts();

            libdnf5::repo::RepoQuery enabled_repos(base);
            enabled_repos.filter_enabled(true);
            enabled_repos.filter_type(libdnf5::repo::Repo::Type::AVAILABLE);
            for (auto & repo : enabled_repos.get_data()) {
                repo->set_sync_strategy(libdnf5::repo::Repo::SyncStrategy::ONLY_CACHE);
                repo->get_config().get_skip_if_unavailable_option().set(libdnf5::Option::Priority::RUNTIME, true);
            }

            ctx.load_repos(installed);
        } catch (...) {
            // Ignores errors when completing available packages, other completions may still work.
        }
    } else if (installed) {
        try {
            base.get_repo_sack()->load_repos(libdnf5::repo::Repo::Type::SYSTEM);
        } catch (...) {
            // Ignores errors when completing installed packages, other completions may still work.
        }
    }

    std::set<std::string> result_set;
    {
        libdnf5::rpm::PackageQuery matched_pkgs_query(base);
        libdnf5::ResolveSpecSettings settings;
        settings.set_ignore_case(false);
        settings.set_with_provides(false);
        settings.set_with_filenames(false);
        settings.set_with_binaries(false);
        matched_pkgs_query.resolve_pkg_spec(pattern + '*', settings, true);

        for (const auto & package : matched_pkgs_query) {
            if (!only_nevras) {
                if (auto name = package.get_name(); name.length() >= pattern.length()) {
                    // The output must not include a package name shorter than the length of the input patter
                    // (we don't want to shorten the user-specified input).
                    result_set.insert(std::move(name));
                }
            }

            result_set.insert(package.get_full_nevra());
        }
    }

    std::vector<std::string> file_paths;
    std::vector<std::string> dir_paths;
    if (paths) {
        if (!file_name_regex) {
            file_name_regex = ".*";
        }
        std::regex regex_pattern(file_name_regex, std::regex_constants::nosubs | std::regex_constants::optimize);
        std::tie(file_paths, dir_paths) = complete_paths(pattern, regex_pattern);
        std::sort(file_paths.begin(), file_paths.end());
        std::sort(dir_paths.begin(), dir_paths.end());
    }

    std::vector<std::string> result;
    result.reserve(file_paths.size() + dir_paths.size() + result_set.size());
    std::move(file_paths.begin(), file_paths.end(), std::back_inserter(result));
    std::move(result_set.begin(), result_set.end(), std::back_inserter(result));
    std::move(dir_paths.begin(), dir_paths.end(), std::back_inserter(result));
    return result;
}

}  // namespace dnf5
