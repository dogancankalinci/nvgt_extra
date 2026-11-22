/* git2.h - libgit2 wrapper plugin header
 *
 * NVGT - NonVisual Gaming Toolkit
 * Copyright (c) 2022-2025 Sam Tupy
 * https://nvgt.dev
 * This software is provided "as-is", without any express or implied warranty. In no event will the authors be held liable for any damages arising from the use of this software.
 * Permission is granted to anyone to use this software for any purpose, including commercial applications, and to alter it and redistribute it freely, subject to the following restrictions:
 * 1. The origin of this software must not be misrepresented; you must not claim that you wrote the original software. If you use this software in a product, an acknowledgment in the product documentation would be appreciated but is not required.
 * 2. Altered source versions must be plainly marked as such, and must not be misrepresented as being the original software.
 * 3. This notice may not be removed or altered from any source distribution.
*/

#include <git2.h>
#include <angelscript.h>
#include <scriptarray.h>
#include <string>
#include <cstring>
#include <vector>

git_strarray as_strarray2git_strarray(CScriptArray* as_array);

class nvgt_git_repository_entry;
class nvgt_git_repository_commit;
class nvgt_git_repository_commit_iterator;
// An open repository and its index, freed when the last of its users lets go: the git_repository object that opened it, and every
// commit and commit iterator made from it, which use the repository through libgit2. close(), or letting go of the
// git_repository, freed both at once, and a commit or iterator still held by the script then used freed memory (get_parent(),
// the iterator's next()).
class nvgt_git_repository_handle {
	int ref_count;
public:
	git_repository* repo;
	git_index* index;
	nvgt_git_repository_handle(git_repository* r, git_index* i) : ref_count(1), repo(r), index(i) {}
	void add_ref() { asAtomicInc(ref_count); }
	void release() {
		if (asAtomicDec(ref_count) < 1) {
			if (index) git_index_free(index);
			if (repo) git_repository_free(repo);
			delete this;
		}
	}
};
class nvgt_git_repository {
	git_repository* repo;
	git_index* index;
	nvgt_git_repository_handle* handle; // owns repo and index, see above
	int ref_count;
	bool busy; // add_all(), update_all() or remove_all() is running a script callback, see begin_callback_operation()
	nvgt_git_repository_handle* begin_callback_operation(asIScriptFunction* match_callback, const std::string& match_callback_payload);
	void end_callback_operation(nvgt_git_repository_handle* h);
public:
	asIScriptFunction* match_callback;
	std::string match_callback_payload;
	nvgt_git_repository();
	void add_ref();
	void release();
	int open(const std::string& path);
	int create(const std::string& path);
	bool close();
	int add(const std::string& path);
	int add_all(CScriptArray* paths, int flags = 0);
	int add_all_cb(CScriptArray* paths, int flags = 0, asIScriptFunction* match_callback = NULL, const std::string& match_callback_payload = "");
	int remove(const std::string& path);
	int remove_all(CScriptArray* paths);
	int remove_all_cb(CScriptArray* paths, asIScriptFunction* match_callback = NULL, const std::string& match_callback_payload = "");
	int update_all(CScriptArray* paths);
	int update_all_cb(CScriptArray* paths, asIScriptFunction* match_callback = NULL, const std::string& match_callback_payload = "");
	nvgt_git_repository_commit* commit_simple(const std::string& author, const std::string& author_email, const std::string& message, const std::string& ref = "HEAD") { return commit(author, author_email, author, author_email, message, ref); }
	nvgt_git_repository_commit* commit(const std::string& author, const std::string& author_email, const std::string& committer, const std::string& committer_email, const std::string& message, const std::string& ref = "HEAD");
	std::string commit_diff(nvgt_git_repository_commit* commit1, nvgt_git_repository_commit* commit2, unsigned int context_lines = 3, unsigned int interhunk_lines = 3, unsigned int flags = 0, unsigned int format = 1, CScriptArray* pathspec = NULL, const std::string& old_prefix = "a", const std::string& new_prefix = "b");
	nvgt_git_repository_commit* commit_lookup(const std::string& id);
	nvgt_git_repository_commit_iterator* commit_iterate(CScriptArray* path_filter = NULL, const std::string& author_filter = "", const std::string& message_filter = "", git_time_t min_time_filter = 0, git_time_t max_time_filter = 0, unsigned int start = 0, unsigned int count = 0);
	nvgt_git_repository_entry* get_entry(unsigned int n);
	nvgt_git_repository_entry* find_entry(const std::string& path);
	int get_entry_count();
	int get_is_empty();
	const std::string get_path();
	const std::string get_workdir();
	bool get_active() { return repo && index; }
};

class nvgt_git_repository_commit {
	int ref_count;
	void get_signatures();
public:
	git_commit* commit;
	std::string committer, committer_email, author, author_email;
	nvgt_git_repository_handle* handle; // the repository the commit belongs to, kept open while the commit lives
	nvgt_git_repository_commit(git_commit* c, nvgt_git_repository_handle* h) : ref_count(1), commit(c), handle(h) { handle->add_ref(); }
	void add_ref() { asAtomicInc(ref_count); }
	void release() { if (asAtomicDec(ref_count) < 1) { git_commit_free(commit); handle->release(); delete this; } }
	git_time_t get_time() { return git_commit_time(commit); } // 64 bits, signed: cut to a uint, a commit made before 1970 or after 2106 got another date
	int get_parent_count() { return git_commit_parentcount(commit); }
	nvgt_git_repository_commit* get_parent(int idx);
	const std::string get_id() { const char* o = git_oid_tostr_s(git_commit_id(commit)); return std::string(o, strlen(o)); }
	const std::string get_message() { const char* m = git_commit_message(commit); if (!m) return ""; return std::string(m, strlen(m)); }
	const std::string get_summary() { const char* s = git_commit_summary(commit); return s ? std::string(s) : ""; } // null when libgit2 can't make it (out of memory)
	const std::string get_body() { const char* b = git_commit_body(commit); if (!b) return ""; return std::string(b, strlen(b)); }
	const std::string get_raw_header() { const char* h = git_commit_raw_header(commit); return std::string(h, strlen(h)); }
	const std::string get_committer() { get_signatures(); return committer; };
	const std::string get_committer_email() { get_signatures(); return committer_email; };
	const std::string get_author() { get_signatures(); return author; };
	const std::string get_author_email() { get_signatures(); return author_email; };
};
class nvgt_git_repository_commit_iterator {
	int ref_count;
	git_revwalk* walker;
	git_diff_options dopts;
	git_pathspec* pspec;
	std::vector<std::string> path_strings; // the path filter's own copy, which dopts.pathspec points into (it pointed into the script's array)
	std::vector<char*> path_pointers;
	std::string author_filter, message_filter;
	git_time_t min_time_filter, max_time_filter;
	int start, count, current_entry, inserted_entries;
	nvgt_git_repository_handle* handle; // the repository walked, kept open while the iterator lives
public:
	nvgt_git_repository_commit* commit;
	nvgt_git_repository_commit_iterator(git_revwalk* w, nvgt_git_repository_handle* h, CScriptArray* path_filter, const std::string& author_filter, const std::string& message_filter, git_time_t min_time_filter, git_time_t max_time_filter, unsigned int start, unsigned int count) : ref_count(1), walker(w), dopts(GIT_DIFF_OPTIONS_INIT), pspec(NULL), author_filter(author_filter), message_filter(message_filter), min_time_filter(min_time_filter), max_time_filter(max_time_filter), start(start), count(count), current_entry(-1), inserted_entries(0), handle(h), commit(NULL) {
		handle->add_ref();
		if (path_filter && path_filter->GetSize() > 0) {
			for (asUINT i = 0; i < path_filter->GetSize(); i++) path_strings.push_back(*(std::string*)path_filter->At(i));
			for (std::string& p : path_strings) path_pointers.push_back(&p[0]);
			dopts.pathspec.strings = path_pointers.data();
			dopts.pathspec.count = path_pointers.size();
			if (git_pathspec_new(&pspec, &dopts.pathspec)) pspec = NULL;
		}
	}
	void add_ref() { asAtomicInc(ref_count); }
	void release() { if (asAtomicDec(ref_count) < 1) { git_revwalk_free(walker); if (pspec) git_pathspec_free(pspec); if (commit) commit->release(); handle->release(); delete this; } }
	nvgt_git_repository_commit* get_commit() { if (commit) commit->add_ref(); return commit; } // null before the first next(), where it crashed
	bool next();
};

// A copy of an index entry's values: it pointed into the index, whose entries are freed by close() or by letting go of the
// repository, and moved or freed by add(), add_all(), remove() and the rest, so a held entry read freed memory.
class nvgt_git_repository_entry {
	unsigned int ctime, mtime, file_size;
	git_oid id;
	std::string path;
	int ref_count;
public:
	nvgt_git_repository_entry(const git_index_entry* e) : ctime(e->ctime.seconds), mtime(e->mtime.seconds), file_size(e->file_size), id(e->id), path(e->path ? e->path : ""), ref_count(1) {}
	void add_ref() { asAtomicInc(ref_count); }
	void release() { if (asAtomicDec(ref_count) < 1) delete this; }
	unsigned int get_ctime() { return ctime; }
	unsigned int get_mtime() { return mtime; }
	unsigned int get_file_size() { return file_size; }
	const std::string get_oid() { const char* o = git_oid_tostr_s(&id); return std::string(o, strlen(o)); }
	const std::string get_path() { return path; }
};

void RegisterGit(asIScriptEngine* engine);
