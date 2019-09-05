// Pegasus Frontend
// Copyright (C) 2017-2019  Mátyás Mustoha
//
// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation, either version 3 of the License, or
// (at your option) any later version.
//
// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
// GNU General Public License for more details.
//
// You should have received a copy of the GNU General Public License
// along with this program. If not, see <http://www.gnu.org/licenses/>.


#include "PegasusMetadataFilter.h"

#include "modeldata/CollectionData.h"
#include "modeldata/GameData.h"
#include "utils/StdHelpers.h"

#include <QDir>
#include <QDirIterator>


namespace {
using FileFilter = providers::pegasus::parser::FileFilter;

bool rx_match(const QRegularExpression& rx, const QString& str) {
    return !rx.pattern().isEmpty() && rx.match(str).hasMatch();
}

// Find all dirs and subdirectories, but ignore 'media'
std::vector<QString> all_valid_subdirs(const QString& filter_dir)
{
    Q_ASSERT(!filter_dir.isEmpty());
    if (Q_UNLIKELY(filter_dir.isEmpty()))
        return {};

    constexpr auto subdir_filters = QDir::Dirs | QDir::Readable | QDir::NoDotAndDotDot;
    constexpr auto subdir_flags = QDirIterator::FollowSymlinks | QDirIterator::Subdirectories;

    std::vector<QString> result;

    QDirIterator dirs_it(filter_dir, subdir_filters, subdir_flags);
    while (dirs_it.hasNext())
        result.emplace_back(dirs_it.next());

    const QString media_dir = filter_dir + QStringLiteral("/media");
    VEC_REMOVE_VALUE(result, media_dir);
    result.emplace_back(filter_dir + QStringLiteral("/")); // added "/" so all entries have base + 1 length

    return result;
}

std::vector<QString> resolve_filelist(const std::vector<QString>& files, const QString& rootdir)
{
    std::vector<QString> can_paths;
    can_paths.reserve(files.size());

    for (const QString& file : files) {
        const QFileInfo finfo(rootdir, file);
        QString can_path = finfo.canonicalFilePath();
        if (!can_path.isEmpty())
            can_paths.emplace_back(std::move(can_path));
    }

    return can_paths;
}

bool file_passes_filter(const QFileInfo& finfo, const FileFilter& filter,
                        const std::vector<QString>& exclude_files)
{
    const QString file_ext = finfo.suffix().toLower();

    const bool exclude = VEC_CONTAINS(filter.exclude.extensions, file_ext)
        || VEC_CONTAINS(exclude_files, finfo.canonicalFilePath())
        || rx_match(filter.exclude.regex, finfo.filePath());
    if (exclude)
        return false;

    const bool include = VEC_CONTAINS(filter.include.extensions, file_ext)
        || rx_match(filter.include.regex, finfo.filePath());
    if (!include)
        return false;

    return true;
}

void accept_filtered_file(const QFileInfo& fileinfo, const modeldata::Collection& parent,
                          providers::SearchContext& sctx)
{
    const QString game_path = fileinfo.canonicalFilePath();
    Q_ASSERT(!game_path.isEmpty());
    if (!sctx.path_to_gameid.count(game_path)) {
        // This means there weren't any game entries with matching file entry
        // in any of the parsed metadata files. There is no existing game data
        // created yet either.
        modeldata::Game game(fileinfo);
        game.launch_cmd = parent.launch_cmd;
        game.launch_workdir = parent.launch_workdir;
        game.relative_basedir = parent.relative_basedir;

        const size_t game_id = sctx.games.size();
        sctx.path_to_gameid.emplace(game_path, game_id);
        sctx.games.emplace(game_id, std::move(game));
    }
    const size_t game_id = sctx.path_to_gameid.at(game_path);
    sctx.collection_childs[parent.name].emplace_back(game_id);

    // When a game was defined earlier than its collection
    modeldata::Game& game = sctx.games.at(game_id);
    if (game.launch_cmd.isEmpty())
        game.launch_cmd = parent.launch_cmd;
    if (game.launch_workdir.isEmpty())
        game.launch_workdir = parent.launch_workdir;
    if (game.relative_basedir.isEmpty())
        game.relative_basedir = parent.relative_basedir;
}
} // namespace


namespace providers {
namespace pegasus {
namespace parser {

FileFilterGroup::FileFilterGroup() = default;

FileFilter::FileFilter(QString collection, QString base_dir)
    : collection_key(std::move(collection))
    , directories({std::move(base_dir)})
{
    Q_ASSERT(!collection_key.isEmpty());
    Q_ASSERT(!directories.front().isEmpty());
}


void tidy_filters(std::vector<FileFilter>& filters)
{
    for (FileFilter& filter : filters) {
        VEC_REMOVE_DUPLICATES(filter.directories);
        VEC_REMOVE_DUPLICATES(filter.include.extensions);
        VEC_REMOVE_DUPLICATES(filter.include.files);
        VEC_REMOVE_DUPLICATES(filter.exclude.extensions);
        VEC_REMOVE_DUPLICATES(filter.exclude.files);
    }
}

void process_filter(const FileFilter& filter, providers::SearchContext& sctx)
{
    constexpr auto entry_filters = QDir::Files | QDir::Dirs | QDir::Readable | QDir::NoDotAndDotDot;
    constexpr auto entry_flags = QDirIterator::FollowSymlinks;

    const modeldata::Collection& collection = sctx.collections.at(filter.collection_key);

    std::vector<QString> all_include_files;
    std::vector<QString> all_exclude_files;
    all_include_files.reserve(filter.directories.size() * filter.include.files.size());
    all_exclude_files.reserve(filter.directories.size() * filter.exclude.files.size());

    for (const QString& filter_dir : filter.directories) {
        const std::vector<QString> dirs_to_check = all_valid_subdirs(filter_dir);
        std::vector<QString> include_files = resolve_filelist(filter.include.files, filter_dir);
        std::vector<QString> exclude_files = resolve_filelist(filter.exclude.files, filter_dir);

        for (const QString& subdir : dirs_to_check) {
            QDirIterator subdir_it(subdir, entry_filters, entry_flags);
            while (subdir_it.hasNext()) {
                subdir_it.next();
                const QFileInfo fileinfo = subdir_it.fileInfo();

                if (file_passes_filter(fileinfo, filter, exclude_files))
                    accept_filtered_file(fileinfo, collection, sctx);
            }
        }

        vec_append_move(all_include_files, std::move(include_files));
        vec_append_move(all_exclude_files, std::move(exclude_files));
    }

    VEC_REMOVE_DUPLICATES(all_include_files);
    VEC_REMOVE_DUPLICATES(all_exclude_files);
    for (const QString& can_path: all_include_files) {
        if (!VEC_CONTAINS(all_exclude_files, can_path))
            accept_filtered_file(QFileInfo(can_path), collection, sctx);
    }
}

void process_filters(const std::vector<FileFilter>& filters, providers::SearchContext& sctx)
{
    for (const FileFilter& filter : filters)
        process_filter(filter, sctx);
}

} // namespace parser
} // namespace pegasus
} // namespace providers
