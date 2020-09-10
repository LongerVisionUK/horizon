#include "pool/package.hpp"
#include "util/util.hpp"
#include "nlohmann/json.hpp"
#include "pool/pool.hpp"
#include "board/board_layers.hpp"
#include "pool/part.hpp"
#include "pool/entity.hpp"
#include "pool/pool_manager.hpp"
#include <glibmm/miscutils.h>
#include <giomm.h>
#include <git2.h>
#include "util/autofree_ptr.hpp"
#include "pool-update/pool-update.hpp"
#include "common/object_descr.hpp"

using namespace horizon;

int diff_file_cb_c(const git_diff_delta *delta, float progress, void *pl)
{
    auto db = reinterpret_cast<SQLite::Database *>(pl);
    SQLite::Query q(*db, "INSERT INTO 'git_files' VALUES (?, ?)");
    q.bind(1, std::string(delta->new_file.path));
    q.bind(2, static_cast<int>(delta->status));
    q.step();
    return 0;
}

static std::string delta_to_string(git_delta_t delta)
{
    switch (delta) {
    case GIT_DELTA_ADDED:
        return "New";

    case GIT_DELTA_MODIFIED:
        return "Modified";

    default:
        return "Unknown (" + std::to_string(static_cast<int>(delta)) + ")";
    }
}

static int count_manufactuer(Pool &pool, const std::string &mfr)
{
    SQLite::Query q(pool.db, "SELECT COUNT(*) FROM parts WHERE manufacturer = ?");
    q.bind(1, mfr);
    q.step();
    return q.get<int>(0);
}

static std::string surround_if(const char *prefix, const char *suffix, const std::string &s, bool cond = true)
{
    if (s.size() && cond)
        return prefix + s + suffix;
    else
        return s;
}

static bool needs_trim(const std::string &s)
{
    return s.size() && (isspace(s.front()) || isspace(s.back()));
}

static const std::string whitespace_warning = "(:warning: has trailing/leading whitespace)";

class PinDirectionMap {
public:
    const std::map<Pin::Direction, std::string> &get()
    {
        if (!m) {
            m = new std::map<Pin::Direction, std::string>;
            for (const auto &it : Pin::direction_names) {
                m->emplace(it.first, it.second);
            }
        }
        return *m;
    }

private:
    std::map<Pin::Direction, std::string> *m = nullptr;
};

static PinDirectionMap pin_direction_map;

static const std::vector<std::string> forbidden_datasheet_domains = {
        "rs-online.com", "digikey.com", "mouser.com", "farnell.com", "octopart.com",
};

static std::optional<std::string> check_datasheet(const std::string &url)
{
    for (const auto &it : forbidden_datasheet_domains) {
        if (url.find(it) != std::string::npos)
            return it;
    }
    return {};
}

int main(int c_argc, char *c_argv[])
{
    Gio::init();
    PoolManager::init();
    git_libgit2_init();


    Glib::OptionContext options;
    options.set_summary("horizon pr review");
    options.set_help_enabled();

    Glib::OptionGroup group("pr-review", "pr-review");

    std::string output_filename;
    Glib::OptionEntry entry;
    entry.set_long_name("output");
    entry.set_short_name('o');
    entry.set_description("output filename");
    group.add_entry_filename(entry, output_filename);

    bool do_pool_update = false;
    Glib::OptionEntry entry_pool_update;
    entry_pool_update.set_long_name("pool-update");
    entry_pool_update.set_short_name('u');
    entry_pool_update.set_description("update pool before generating review");
    group.add_entry(entry_pool_update, do_pool_update);


    std::vector<std::string> filenames;
    Glib::OptionEntry entry_f;
    entry_f.set_long_name(G_OPTION_REMAINING);
    entry_f.set_short_name(' ');
    entry_f.set_description("Pool directory");
    group.add_entry_filename(entry_f, filenames);

    options.set_main_group(group);
    options.parse(c_argc, c_argv);

    if (filenames.size() != 1) {
        std::cerr << "pool path not specified" << std::endl;
        return 1;
    }

    if (output_filename.size() == 0) {
        std::cerr << "output filename not specified" << std::endl;
        return 1;
    }

    auto ofs = make_ofstream(output_filename);

    auto pool_base_path = filenames.at(0);
    if (do_pool_update) {
        std::list<std::pair<std::string, std::string>> errors;
        pool_update(
                pool_base_path,
                [&errors](PoolUpdateStatus status, std::string filename, std::string detail) {
                    if (status == PoolUpdateStatus::FILE_ERROR) {
                        errors.emplace_back(filename, detail);
                    }
                },
                true);
        if (errors.size()) {
            ofs << "# Pool update encountered errors\n";
            for (const auto &[filename, detail] : errors) {
                ofs << " -" << filename << " " << detail << "\n";
            }
            return 1;
        }
    }


    Pool pool(pool_base_path);


    autofree_ptr<git_repository> repo(git_repository_free);
    if (git_repository_open(&repo.ptr, pool_base_path.c_str()) != 0) {
        throw std::runtime_error("error opening repo");
    }

    autofree_ptr<git_object> treeish_master(git_object_free);
    if (git_revparse_single(&treeish_master.ptr, repo, "master") != 0) {
        throw std::runtime_error("error finding master branch");
    }

    autofree_ptr<git_object> otree_master(git_object_free);
    if (git_object_peel(&otree_master.ptr, treeish_master, GIT_OBJ_TREE) != 0) {
        throw std::runtime_error("error peeling master");
    }

    autofree_ptr<git_tree> tree_master(git_tree_free);


    if (git_tree_lookup(&tree_master.ptr, repo, git_object_id(otree_master)) != 0) {
        throw std::runtime_error("error finding master tree");
    }

    pool.db.execute("CREATE TEMP TABLE 'git_files' ('git_filename' TEXT NOT NULL, 'status' INT NOT NULL);");
    pool.db.execute("BEGIN");
    {
        autofree_ptr<git_diff> diff(git_diff_free);
        git_diff_tree_to_workdir_with_index(&diff.ptr, repo, tree_master, nullptr);
        git_diff_foreach(diff, &diff_file_cb_c, nullptr, nullptr, nullptr, &pool.db);
    }
    pool.db.execute("COMMIT");
    pool.db.execute(
            "CREATE TEMP VIEW git_files_view AS "
            "SELECT type, uuid, name, filename, status FROM git_files INNER JOIN "
            "(SELECT type, uuid, name, filename FROM all_items_view UNION ALL SELECT DISTINCT 'model_3d' AS type, "
            "'00000000-0000-0000-0000-000000000000' as uuid, '' as name, model_filename as filename FROM models) "
            "ON filename=git_filename");

    {
        ofs << "# Items in this PR\n";
        ofs << "| State | Type | Name | Filename |\n";
        ofs << "| --- | --- | --- | --- |\n";
        SQLite::Query q(pool.db, "SELECT type, uuid, name, filename, status FROM git_files_view");
        while (q.step()) {
            auto type = object_type_lut.lookup(q.get<std::string>(0));
            const auto name = q.get<std::string>(2);
            ofs << "|" << delta_to_string(static_cast<git_delta_t>(q.get<int>(4))) << " | "
                << object_descriptions.at(type).name << " | " << name;
            if (needs_trim(name))
                ofs << " " << whitespace_warning;

            ofs << " | " << q.get<std::string>(3) << "\n";
        }
        ofs << "\n";
    }

    {
        SQLite::Query q(pool.db,
                        "SELECT git_filename FROM git_files LEFT JOIN all_items_view ON "
                        "filename=git_filename WHERE filename is NULL");
        bool first = true;
        while (q.step()) {
            if (first) {
                ofs << "# Non-items\n";
                first = false;
            }
            ofs << " - " << q.get<std::string>(0) << "\n";
        }
        ofs << "\n";
    }

    pool.db.execute(
            "CREATE TEMP VIEW top_parts AS "
            "SELECT git_files_view.uuid AS part_uuid FROM git_files_view "
            "LEFT JOIN parts ON git_files_view.uuid = parts.uuid "
            "LEFT JOIN git_files_view AS gfv ON gfv.uuid = parts.base "
            "WHERE (gfv.uuid IS NULL OR parts.base = '00000000-0000-0000-0000-000000000000') "
            "AND git_files_view.type = 'part'");
    pool.db.execute(
            "CREATE TEMP VIEW parts_tree AS "
            "WITH RECURSIVE where_used(typex, uuidx, level, root) AS ( SELECT 'part', part_uuid, 0, "
            "part_uuid from top_parts UNION "
            "SELECT dep_type, dep_uuid, level+1, root FROM dependencies, where_used "
            "WHERE dependencies.type = where_used.typex "
            "AND dependencies.uuid = where_used.uuidx) "
            "SELECT where_used.typex AS type, all_items_view.name, level, ( "
            "CASE where_used.typex "
            "WHEN 'part' THEN 0 "
            "WHEN 'entity' THEN 1 "
            "WHEN 'unit' THEN 2 "
            "WHEN 'package' THEN 4 "
            "WHEN 'padstack' THEN 6 "
            "ELSE -1 END) AS type_order, "
            "(SELECT COUNT(*) from git_files_view "
            "WHERE git_files_view.uuid = where_used.uuidx AND "
            "git_files_view.type = where_used.typex) AS in_pr, "
            "where_used.uuidx AS uuid, root "
            "FROM where_used "
            "LEFT JOIN all_items_view "
            "ON where_used.typex = all_items_view.type "
            "AND where_used.uuidx = all_items_view.uuid");
    pool.db.execute(
            "CREATE TEMP VIEW derived_parts_tree AS "
            "WITH RECURSIVE where_used(uuidx, level) AS ( SELECT part_uuid, 0 "
            "FROM top_parts UNION "
            "SELECT parts.uuid, level+1 FROM parts, where_used "
            "WHERE parts.base = where_used.uuidx) "
            "SELECT parts.MPN, level, "
            "(SELECT COUNT(*) from git_files_view "
            "WHERE git_files_view.uuid = where_used.uuidx AND "
            "git_files_view.type = 'part') AS in_pr, "
            "where_used.uuidx AS uuid "
            "FROM where_used "
            "LEFT JOIN parts ON where_used.uuidx = parts.uuid");
    pool.db.execute(
            "CREATE TEMP VIEW all_parts_tree AS "
            "SELECT * FROM ("
            "SELECT * FROM parts_tree "
            "UNION SELECT 'model_3d', model_filename, level+1, 5, in_pr, '', root FROM parts_tree "
            "INNER JOIN models ON (models.package_uuid = uuid and type = 'package') "
            "UNION SELECT 'symbol', symbols.name, level+1, 3, in_pr, symbols.uuid, root FROM parts_tree "
            "INNER JOIN symbols ON (symbols.unit = parts_tree.uuid AND type = 'unit')) "
            "ORDER BY root, type_order, level");

    {
        ofs << "# Parts overview (excluding derived)\n";
        ofs << "Bold items are from this PR\n";
        SQLite::Query q(pool.db, "SELECT * FROM all_parts_tree");
        while (q.step()) {
            const auto type = object_type_lut.lookup(q.get<std::string>(0));
            const auto name = q.get<std::string>(1);
            const auto level = q.get<int>(2);
            const auto from_pr = q.get<int>(4);
            for (int i = 0; i < level; i++) {
                ofs << "  ";
            }
            ofs << "- " << surround_if("**", "**", object_descriptions.at(type).name + " " + name, from_pr) << "\n";
        }
    }

    {
        bool first = true;
        SQLite::Query q(pool.db,
                        "SELECT git_files_view.type, git_files_view.name FROM git_files_view "
                        "LEFT JOIN all_parts_tree ON git_files_view.uuid = all_parts_tree.uuid "
                        "AND git_files_view.type = all_parts_tree.type "
                        "LEFT JOIN derived_parts_tree ON git_files_view.uuid = derived_parts_tree.uuid "
                        "AND git_files_view.type = 'part' "
                        "WHERE all_parts_tree.uuid IS NULL AND derived_parts_tree.uuid IS NULL");
        while (q.step()) {
            if (first)
                ofs << "# Items not associated with any part\n";
            first = false;
            auto type = object_type_lut.lookup(q.get<std::string>(0));

            ofs << " - " << object_descriptions.at(type).name << " " << q.get<std::string>(1) << "\n";
        }
    }


    {
        int n_derived = 0;
        {
            SQLite::Query q(pool.db,
                            "SELECT COUNT(*) FROM git_files_view "
                            "LEFT JOIN parts ON git_files_view.uuid = parts.uuid AND git_files_view.type = 'part' "
                            "WHERE parts.base != '00000000-0000-0000-0000-000000000000'");
            if (q.step()) {
                n_derived = q.get<int>(0);
            }
        }

        if (n_derived) {
            {
                ofs << "# Derived parts\n";
                ofs << "Bold items are from this PR\n";
                SQLite::Query q(pool.db, "SELECT * FROM derived_parts_tree");
                while (q.step()) {
                    const auto name = q.get<std::string>(0);
                    const auto level = q.get<int>(1);
                    const auto from_pr = q.get<int>(2);
                    for (int i = 0; i < level; i++) {
                        ofs << "  ";
                    }
                    ofs << "- " << surround_if("**", "**", name, from_pr) << "\n";
                }
            }
            {
                ofs << "# Parts table\n";
                ofs << "Values in italic are inherited\n";
                ofs << "| MPN | Value | Manufacturer | Datasheet | Description | Tags |\n";
                ofs << "| --- | ----- | ------------ | --------- | ----------- | ---- |\n";
                SQLite::Query q(pool.db, "SELECT uuid FROM derived_parts_tree");
                while (q.step()) {
                    const auto &part = *pool.get_part(q.get<std::string>(0));

                    auto get_attr = [&part](Part::Attribute attr) {
                        return surround_if("*", "*", part.get_attribute(attr), part.attributes.at(attr).first);
                    };

                    ofs << "| " << get_attr(Part::Attribute::MPN);
                    ofs << "| " << get_attr(Part::Attribute::VALUE);
                    ofs << "| " << get_attr(Part::Attribute::MANUFACTURER);
                    ofs << "| " << get_attr(Part::Attribute::DATASHEET);
                    ofs << "| " << get_attr(Part::Attribute::DESCRIPTION);
                    {
                        SQLite::Query qtags(pool.db, "SELECT tags FROM tags_view WHERE type = 'part' AND uuid = ?");
                        qtags.bind(1, part.uuid);
                        if (qtags.step()) {
                            ofs << "| " << surround_if("*", "*", qtags.get<std::string>(0), part.inherit_tags);
                        }
                    }
                    ofs << "\n";
                }
            }
        }
    }

    ofs << "# Details\n";
    ofs << "## Parts\n";
    {
        SQLite::Query q(pool.db, "SELECT uuid FROM derived_parts_tree");
        while (q.step()) {
            const auto &part = *pool.get_part(q.get<std::string>(0));
            ofs << "### " << part.get_MPN() << "\n";
            if (part.base)
                ofs << "Inerhits from " << part.base->get_MPN() << "\n";
            ofs << "| Attribute | Value |\n";
            ofs << "| --- | --- |\n";
            static const std::vector<std::pair<Part::Attribute, std::string>> attrs = {
                    {Part::Attribute::MPN, "MPN"},
                    {Part::Attribute::VALUE, "Value"},
                    {Part::Attribute::MANUFACTURER, "Manufacturer"},
                    {Part::Attribute::DATASHEET, "Datasheet"},
                    {Part::Attribute::DESCRIPTION, "Description"},
            };
            for (const auto &[attr, attr_name] : attrs) {
                const auto val = part.get_attribute(attr);
                ofs << "|" << attr_name << " | " << val;
                if (needs_trim(val))
                    ofs << " " << whitespace_warning;
                if (attr == Part::Attribute::MANUFACTURER) {
                    ofs << " (" << count_manufactuer(pool, val) << " other parts)";
                }
                else if (attr == Part::Attribute::DATASHEET) {
                    auto r = check_datasheet(val);
                    if (r) {
                        ofs << " (:warning: forbidden domain " << *r << ", use primary source)";
                    }
                }
                else if (attr == Part::Attribute::VALUE) {
                    if (val == part.get_attribute(Part::Attribute::MPN))
                        ofs << " (:warning: leave value blank if it's identical to MPN)";
                }
                if (part.attributes.at(attr).first) {
                    ofs << " (inherited)";
                }
                ofs << "\n";
            }
            {
                SQLite::Query qtags(pool.db, "SELECT tags FROM tags_view WHERE type = 'part' AND uuid = ?");
                qtags.bind(1, part.uuid);
                if (qtags.step()) {
                    ofs << "|Tags | " << qtags.get<std::string>(0) << "\n";
                }
            }
        }
    }
    ofs << "## Entities\n";
    {
        SQLite::Query q(pool.db, "SELECT uuid from git_files_view where type = 'entity'");
        while (q.step()) {
            const auto &entity = *pool.get_entity(q.get<std::string>(0));
            ofs << "### " << entity.name << "\n";
            ofs << "| Attribute | Value |\n";
            ofs << "| --- | --- |\n";
            ofs << "|Manufacturer | " << entity.manufacturer << " (" << count_manufactuer(pool, entity.manufacturer)
                << " other parts)\n";
            ofs << "|Prefix | " << entity.prefix << "\n";
            {
                SQLite::Query qtags(pool.db, "SELECT tags FROM tags_view WHERE type = 'entity' AND uuid = ?");
                qtags.bind(1, entity.uuid);
                if (qtags.step()) {
                    ofs << "|Tags | " << qtags.get<std::string>(0) << "\n";
                }
            }
            ofs << "\n";

            std::vector<const Gate *> gates_sorted;
            for (const auto &it : entity.gates) {
                gates_sorted.emplace_back(&it.second);
            }
            if (gates_sorted.size()) {
                ofs << "| Gate | Suffix | Swap group | Unit |\n";
                ofs << "| --- | --- | --- | --- |\n";
                std::sort(gates_sorted.begin(), gates_sorted.end(),
                          [](const auto a, const auto b) { return strcmp_natural(a->name, b->name) < 0; });
                for (auto gate : gates_sorted) {
                    ofs << "|" << gate->name << " | " << gate->suffix << " | " << gate->swap_group << " | "
                        << gate->unit->name << "\n";
                }
            }
            else {
                ofs << ":warning: Entity has no gates!\n";
            }
        }
    }
    ofs << "## Units\n";
    {
        SQLite::Query q(pool.db, "SELECT DISTINCT uuid from git_files_view where type = 'unit'");
        while (q.step()) {
            const auto &unit = *pool.get_unit(q.get<std::string>(0));
            ofs << "### " << unit.name << "\n";
            ofs << "| Attribute | Value |\n";
            ofs << "| --- | --- |\n";
            ofs << "|Manufacturer | " << unit.manufacturer << " (" << count_manufactuer(pool, unit.manufacturer)
                << " other parts)\n";

            ofs << "\n";

            std::vector<const Pin *> pins_sorted;
            for (const auto &it : unit.pins) {
                pins_sorted.emplace_back(&it.second);
            }
            if (pins_sorted.size()) {
                ofs << "| Pin | Direction | Alternate names |\n";
                ofs << "| --- | --- | --- |\n";
                std::sort(pins_sorted.begin(), pins_sorted.end(), [](const auto a, const auto b) {
                    return strcmp_natural(a->primary_name, b->primary_name) < 0;
                });
                for (auto pin : pins_sorted) {
                    std::string alts;
                    for (const auto &it_alt : pin->names) {
                        alts += it_alt + ", ";
                    }
                    if (alts.size()) {
                        alts.pop_back();
                        alts.pop_back();
                    }
                    ofs << "|" << pin->primary_name << " | " << pin_direction_map.get().at(pin->direction) << " | "
                        << alts << "\n";
                }
            }
            else {
                ofs << ":warning: Unit has no pins!\n";
            }
        }
    }


    return 0;
}
