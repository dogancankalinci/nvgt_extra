/* git.cpp - libgit2 wrapper plugin code
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

#include "../../src/nvgt_plugin.h"
#include "git.h"
#include <git2/sys/errors.h>

static bool libgit2_inited = false;
static asIScriptEngine* g_ScriptEngine = NULL;

int nvgt_git_default_match_callback(const char* path, const char* matched, void* payload) {
	nvgt_git_repository* repo = (nvgt_git_repository*)payload;
	if (!repo->match_callback) return 0;
	asIScriptContext* ACtx = asGetActiveContext();
	bool new_context = !ACtx || ACtx->PushState() < 0;
	asIScriptContext* ctx = new_context ? g_ScriptEngine->RequestContext() : ACtx;
	if (!ctx) return GIT_EUSER;
	// The arguments are the callback's three, from 0 (repo, path, user_data): they were set from 1, so the repository went in for the
	// path, the path for the user data, and the user data nowhere. The matched pathspec isn't passed: it was copied with the length of
	// the path, reading past its end.
	std::string path_str(path ? path : "");
	int ret = GIT_EUSER;
	if (ctx->Prepare(repo->match_callback) >= 0 && ctx->SetArgObject(0, repo) >= 0 && ctx->SetArgAddress(1, &path_str) >= 0 && ctx->SetArgAddress(2, &repo->match_callback_payload) >= 0) {
		// A nested execution can't be left suspended (a sleep() or yield() of the context manager in the callback): popped while
		// suspended, the callback's frames stayed where PopState() expects the nested call's marker. It is resumed until it ends, as
		// NVGT's own callbacks do (execute_nested()).
		int r;
		while ((r = ctx->Execute()) == asEXECUTION_SUSPENDED) {}
		if (r == asEXECUTION_FINISHED) ret = (int)ctx->GetReturnDWord();
	}
	if (new_context) g_ScriptEngine->ReturnContext(ctx);
	else {
		// exit() in the callback aborts the nested execution, which consumes the abort: it is passed on to the script that called
		// add_all() and the rest, which went on running after exit() (NVGT's pop_nested_state()).
		bool aborted = ctx->GetState() == asEXECUTION_ABORTED;
		ctx->PopState();
		if (aborted) ctx->Abort();
	}
	return ret;
}
inline git_strarray as_strarray2git_strarray(CScriptArray* as_array) {
	git_strarray strarray;
	strarray.count = as_array->GetSize();
	strarray.strings = (char**)malloc(strarray.count * sizeof(char*));
	for (int i = 0; i < strarray.count; i++)
		strarray.strings[i] = (char*)(*(std::string*)as_array->At(i)).c_str();
	return strarray;
}


nvgt_git_repository::nvgt_git_repository() : repo(NULL), index(NULL), handle(NULL), ref_count(1), busy(false), match_callback(NULL) {
	if (!libgit2_inited) {
		git_libgit2_init();
		libgit2_inited = true;
	}
}
void nvgt_git_repository::add_ref() {
	asAtomicInc(ref_count);
}
void nvgt_git_repository::release() {
	if (asAtomicDec(ref_count) < 1) {
		close();
		delete this;
	}
}
int nvgt_git_repository::open(const std::string& path) {
	if (repo) return GIT_EEXISTS;
	int ret = git_repository_open(&repo, path.c_str());
	if (ret == GIT_OK) {
		git_repository_index(&index, repo);
		handle = new nvgt_git_repository_handle(repo, index);
	} else repo = NULL;
	return ret;
}
int nvgt_git_repository::create(const std::string& path) {
	if (repo) return GIT_EEXISTS;
	int ret = git_repository_init(&repo, path.c_str(), false);
	if (ret == GIT_OK) {
		git_repository_index(&index, repo);
		handle = new nvgt_git_repository_handle(repo, index);
	} else repo = NULL;
	return ret;
}
bool nvgt_git_repository::close() {
	if (!index && !repo) return false;
	// The repository and its index are freed when the last commit, iterator or running add_all() made from them lets go (see
	// nvgt_git_repository_handle).
	index = NULL;
	repo = NULL;
	if (handle) handle->release();
	handle = NULL;
	return true;
}
// Refuses a change to the index while add_all(), update_all() or remove_all() runs a script callback over it, see below.
static int refuse_busy() {
	git_error_set_str(GIT_ERROR_INVALID, "the index can't be changed or committed from an add_all, update_all or remove_all callback");
	return GIT_ELOCKED;
}
// add_all(), update_all() and remove_all() with a callback run script code in the middle of libgit2's walk over the index. A callback
// that changed the same index (add(), remove(), another add_all()...) moved or freed the entry being walked (remove_all() reads it
// again after the callback), and one that closed the repository or let go of the last reference to it freed the index under the walk.
// While the callback can run, the methods that change the index refuse with GIT_ELOCKED, and the index, its repository and this
// object are held until the walk ends.
nvgt_git_repository_handle* nvgt_git_repository::begin_callback_operation(asIScriptFunction* callback, const std::string& payload) {
	add_ref();
	handle->add_ref();
	busy = true;
	match_callback = callback;
	match_callback_payload = payload;
	return handle;
}
void nvgt_git_repository::end_callback_operation(nvgt_git_repository_handle* h) {
	match_callback = NULL;
	busy = false;
	h->release();
	release(); // can let go of this object: nothing may use it after this call
}
// The paths arrays are declared string[]@, a handle whose reference the function is given to let go of: they were kept, so every call
// leaked its array. So is the callback's, which was let go of whether or not there was one (a null callback crashed).
static int release_args(int ret, CScriptArray* paths, asIScriptFunction* callback = NULL) {
	if (paths) paths->Release();
	if (callback) callback->Release();
	return ret;
}
int nvgt_git_repository::add(const std::string& path) {
	if (!index) return GIT_ERROR;
	if (busy) return refuse_busy();
	return git_index_add_bypath(index, path.c_str());
}
int nvgt_git_repository::add_all(CScriptArray* paths, int flags) {
	if (!index || !paths) return release_args(GIT_ERROR, paths);
	if (busy) return release_args(refuse_busy(), paths);
	git_strarray paths_list = as_strarray2git_strarray(paths);
	// libgit2 visits the files that differ from the index, as git add -A does. A filter that let only changed and new ones through kept
	// deleted files in the index, and ignored files out of it with GIT_INDEX_ADD_FORCE too.
	int ret = git_index_add_all(index, &paths_list, flags, NULL, NULL);
	free(paths_list.strings);
	return release_args(ret, paths);
}
int nvgt_git_repository::add_all_cb(CScriptArray* paths, int flags, asIScriptFunction* match_callback, const std::string& match_callback_payload) {
	if (!index || !paths) return release_args(GIT_ERROR, paths, match_callback);
	if (busy) return release_args(refuse_busy(), paths, match_callback);
	git_strarray paths_list = as_strarray2git_strarray(paths);
	nvgt_git_repository_handle* h = begin_callback_operation(match_callback, match_callback_payload);
	int ret = git_index_add_all(h->index, &paths_list, flags, match_callback ? nvgt_git_default_match_callback : NULL, this);
	end_callback_operation(h);
	free(paths_list.strings);
	return release_args(ret, paths, match_callback);
}
int nvgt_git_repository::remove(const std::string& path) {
	if (!index) return GIT_ERROR;
	if (busy) return refuse_busy();
	return git_index_remove_bypath(index, path.c_str());
}
int nvgt_git_repository::remove_all(CScriptArray* paths) {
	if (!index || !paths) return release_args(GIT_ERROR, paths);
	if (busy) return release_args(refuse_busy(), paths);
	git_strarray paths_list = as_strarray2git_strarray(paths);
	// Every entry that matches is removed, whatever the state of its file, as git rm --cached does. Only the entries of files changed in
	// the working directory were, so a clean or deleted file's never left the index.
	int ret = git_index_remove_all(index, &paths_list, NULL, NULL);
	free(paths_list.strings);
	return release_args(ret, paths);
}
int nvgt_git_repository::remove_all_cb(CScriptArray* paths, asIScriptFunction* match_callback, const std::string& match_callback_payload) {
	if (!index || !paths) return release_args(GIT_ERROR, paths, match_callback);
	if (busy) return release_args(refuse_busy(), paths, match_callback);
	git_strarray paths_list = as_strarray2git_strarray(paths);
	nvgt_git_repository_handle* h = begin_callback_operation(match_callback, match_callback_payload);
	int ret = git_index_remove_all(h->index, &paths_list, match_callback ? nvgt_git_default_match_callback : NULL, this);
	end_callback_operation(h);
	free(paths_list.strings);
	return release_args(ret, paths, match_callback);
}
int nvgt_git_repository::update_all(CScriptArray* paths) {
	if (!index || !paths) return release_args(GIT_ERROR, paths);
	if (busy) return release_args(refuse_busy(), paths);
	git_strarray paths_list = as_strarray2git_strarray(paths);
	// libgit2 visits the tracked files that differ from the index: changed ones are updated and deleted ones removed, as git add -u does.
	// A filter that let only files changed in the working directory through kept deleted files in the index.
	int ret = git_index_update_all(index, &paths_list, NULL, NULL);
	free(paths_list.strings);
	return release_args(ret, paths);
}
int nvgt_git_repository::update_all_cb(CScriptArray* paths, asIScriptFunction* match_callback, const std::string& match_callback_payload) {
	if (!index || !paths) return release_args(GIT_ERROR, paths, match_callback);
	if (busy) return release_args(refuse_busy(), paths, match_callback);
	git_strarray paths_list = as_strarray2git_strarray(paths);
	nvgt_git_repository_handle* h = begin_callback_operation(match_callback, match_callback_payload);
	int ret = git_index_update_all(h->index, &paths_list, match_callback ? nvgt_git_default_match_callback : NULL, this);
	end_callback_operation(h);
	free(paths_list.strings);
	return release_args(ret, paths, match_callback);
}
nvgt_git_repository_commit* nvgt_git_repository::commit(const std::string& author, const std::string& author_email, const std::string& committer, const std::string& committer_email, const std::string& message, const std::string& commit_ref) {
	git_oid commit_oid, tree_oid;
	git_tree* tree;
	git_object* parent = NULL;
	git_reference* ref = NULL;
	git_signature* signature_author;
	git_signature* signature_committer;
	if (!repo || !index) return NULL; // closed: the commit made below took the repository's handle, which closing let go of
	if (busy) { // writing the tree from an add_all(), update_all() or remove_all() callback can reorder the index under the walk
		refuse_busy();
		return NULL;
	}
	int r = git_revparse_ext(&parent, &ref, repo, commit_ref.c_str());
	if (r != GIT_OK && r != GIT_ENOTFOUND) return NULL;
	if (git_index_write_tree(&tree_oid, index)) {
		git_object_free(parent);
		git_reference_free(ref);
		return NULL;
	}
	if (git_index_write(index)) {
		git_object_free(parent);
		git_reference_free(ref);
		return NULL;
	}
	if (git_tree_lookup(&tree, repo, &tree_oid)) {
		git_object_free(parent);
		git_reference_free(ref);
		return NULL;
	}
	if (git_signature_now(&signature_author, author.c_str(), author_email.c_str())) {
		git_tree_free(tree);
		git_object_free(parent);
		git_reference_free(ref);
		return NULL;
	}
	if (git_signature_now(&signature_committer, committer.c_str(), committer_email.c_str())) {
		git_signature_free(signature_author);
		git_tree_free(tree);
		git_object_free(parent);
		git_reference_free(ref);
		return NULL;
	}
	git_buf clean_message = GIT_BUF_INIT;
	git_message_prettify(&clean_message, message.c_str(), false, 0);
	r = git_commit_create_v(&commit_oid, repo, commit_ref.c_str(), signature_author, signature_committer, NULL, clean_message.ptr ? clean_message.ptr : message.c_str(), tree, parent ? 1 : 0, parent);
	git_commit* commit;
	nvgt_git_repository_commit* ret = NULL;
	if (!r && !git_commit_lookup(&commit, repo, &commit_oid)) ret = new nvgt_git_repository_commit(commit, handle);
	git_signature_free(signature_committer);
	git_signature_free(signature_author);
	git_tree_free(tree);
	git_object_free(parent);
	git_reference_free(ref);
	if (clean_message.ptr) git_buf_dispose(&clean_message);
	return ret;
}
std::string nvgt_git_repository::commit_diff(nvgt_git_repository_commit* commit1, nvgt_git_repository_commit* commit2, unsigned int context_lines, unsigned int interhunk_lines, unsigned int flags, unsigned int format, CScriptArray* pathspec, const std::string& old_prefix, const std::string& new_prefix) {
	if (!commit1 || !commit2 || !repo) return "";
	git_tree* tree1 = NULL, * tree2 = NULL;
	// pathspec is declared string[]@+, whose reference AngelScript keeps and lets go of: it was let go of here a second time when a
	// commit's tree couldn't be read (the array freed under the script).
	if (git_commit_tree(&tree1, commit1->commit))
		return "";
	if (git_commit_tree(&tree2, commit2->commit)) {
		git_tree_free(tree1);
		return "";
	}
	git_diff* diff = NULL;
	git_diff_options opts = GIT_DIFF_OPTIONS_INIT;
	if (pathspec && pathspec->GetSize() > 0)
		opts.pathspec = as_strarray2git_strarray(pathspec);
	opts.flags = flags;
	opts.context_lines = context_lines;
	opts.interhunk_lines = interhunk_lines;
	opts.old_prefix = old_prefix.c_str();
	opts.new_prefix = new_prefix.c_str();
	if (git_diff_tree_to_tree(&diff, repo, tree1, tree2, &opts)) {
		if (tree1) git_tree_free(tree1);
		git_tree_free(tree2);
		if (opts.pathspec.strings) free(opts.pathspec.strings);
		return "";
	}
	git_buf output = GIT_BUF_INIT;
	std::string ret;
	if (!git_diff_to_buf(&output, diff, (git_diff_format_t)format))
		ret = std::string(output.ptr, output.size);
	git_buf_dispose(&output);
	git_diff_free(diff); // it was never freed
	if (tree1) git_tree_free(tree1);
	git_tree_free(tree2);
	if (opts.pathspec.strings) free(opts.pathspec.strings);
	return ret;
}
nvgt_git_repository_commit* nvgt_git_repository::commit_lookup(const std::string& id) {
	git_oid oid;
	if (!repo || git_oid_fromstr(&oid, id.c_str())) return NULL;
	git_commit* c;
	if (git_commit_lookup(&c, repo, &oid)) return NULL;
	return new nvgt_git_repository_commit(c, handle);
}
nvgt_git_repository_commit_iterator* nvgt_git_repository::commit_iterate(CScriptArray* path_filter, const std::string& author_filter, const std::string& message_filter, git_time_t min_time_filter, git_time_t max_time_filter, unsigned int start, unsigned int count) {
	git_revwalk* w = NULL;
	if (!repo || git_revwalk_new(&w, repo)) {
		if (path_filter) path_filter->Release();
		return NULL;
	}
	git_revwalk_push_head(w);
	// The iterator copies the paths, and the array, given as string[]@ for this function to let go of, is let go of here: it was kept
	// for good, and the iterator pointed into its strings (which the script could still change or free).
	nvgt_git_repository_commit_iterator* it = new nvgt_git_repository_commit_iterator(w, handle, path_filter, author_filter, message_filter, min_time_filter, max_time_filter, start, count);
	if (path_filter) path_filter->Release();
	return it;
}
nvgt_git_repository_entry* nvgt_git_repository::get_entry(unsigned int n) {
	if (!index) return NULL;
	const git_index_entry* entry = git_index_get_byindex(index, n);
	if (!entry) return NULL;
	return new nvgt_git_repository_entry(entry);
}
nvgt_git_repository_entry* nvgt_git_repository::find_entry(const std::string& path) {
	if (!index) return NULL;
	size_t pos;
	if (git_index_find(&pos, index, path.c_str()) != GIT_OK) return NULL;
	return get_entry(pos);
}
int nvgt_git_repository::get_entry_count() {
	if (!index) return GIT_ENOTFOUND;
	return git_index_entrycount(index);
}
int nvgt_git_repository::get_is_empty() {
	if (!repo) return GIT_ENOTFOUND;
	return git_repository_is_empty(repo);
}
const std::string nvgt_git_repository::get_path() {
	if (!repo) return "";
	const char* p = git_repository_path(repo);
	return std::string(p, strlen(p));
}
const std::string nvgt_git_repository::get_workdir() {
	if (!repo) return "";
	const char* p = git_repository_workdir(repo);
	return p ? std::string(p) : ""; // null for a bare repository, which strlen() was given
}
nvgt_git_repository* new_git_repository() {
	return new nvgt_git_repository();
}

void nvgt_git_repository_commit::get_signatures() {
	if (committer != "") return;
	const git_signature* sig_committer = git_commit_committer(commit);
	if (sig_committer != NULL) {
		committer = sig_committer->name;
		committer_email = sig_committer->email;
	}
	const git_signature* sig_author = git_commit_author(commit);
	if (sig_author != NULL) {
		author = sig_author->name;
		author_email = sig_author->email;
	}
}
nvgt_git_repository_commit* nvgt_git_repository_commit::get_parent(int idx) {
	git_commit* c = NULL;
	if (git_commit_parent(&c, commit, idx) != GIT_OK) return NULL;
	return new nvgt_git_repository_commit(c, handle);
}
bool nvgt_git_repository_commit_iterator::next() {
	git_oid c_oid;
	git_commit* c = NULL;
	bool found_commit = false;
	while (git_revwalk_next(&c_oid, walker) == GIT_OK) {
		// A commit the filters skipped was never freed, nor were the parents and trees of a commit whose diff couldn't be made.
		if (c) git_commit_free(c);
		c = NULL;
		if (count > 0 && inserted_entries >= count) return false; // it walked the rest of the history, diffing each commit, to find none
		if (git_commit_lookup(&c, git_revwalk_repository(walker), &c_oid) != GIT_OK) return false;
		if (dopts.pathspec.count) {
			int parents = git_commit_parentcount(c);
			git_tree* tree1, * tree2;
			if (parents == 0) {
				if (git_commit_tree(&tree2, c)) continue;
				bool skip = false;
				skip = git_pathspec_match_tree(NULL, tree2, GIT_PATHSPEC_NO_MATCH_ERROR, pspec);
				git_tree_free(tree2);
				if (skip) continue;
			} else {
				int unmatched = parents;
				for (int i = 0; i < parents; i++) {
					git_commit* parent;
					if (git_commit_parent(&parent, c, i)) continue;
					git_diff* diff;
					if (git_commit_tree(&tree1, parent)) { git_commit_free(parent); continue; }
					if (git_commit_tree(&tree2, c)) { git_tree_free(tree1); git_commit_free(parent); continue; }
					if (git_diff_tree_to_tree(&diff, git_commit_owner(c), tree1, tree2, &dopts)) { git_tree_free(tree1); git_tree_free(tree2); git_commit_free(parent); continue; }
					int deltas = git_diff_num_deltas(diff);
					git_diff_free(diff);
					git_tree_free(tree1);
					git_tree_free(tree2);
					git_commit_free(parent);
					if (deltas > 0) unmatched--;
				}
				if (unmatched > 0) continue;
			}
		}
		const git_signature* sig = git_commit_author(c);
		bool author_match = true;
		if ((min_time_filter > 0 || max_time_filter > 0 || author_filter != "") && !sig) continue;
		if (min_time_filter > 0 && sig->when.time < min_time_filter || max_time_filter > 0 && max_time_filter > min_time_filter && sig->when.time > max_time_filter) continue;
		if (author_filter != "" && strstr(sig->name, author_filter.c_str()) == NULL && strstr(sig->email, author_filter.c_str()) == NULL) author_match = false;
		sig = git_commit_committer(c);
		if (!sig && author_filter != "") continue;
		if (!author_match && author_filter != "" && strstr(sig->name, author_filter.c_str()) == NULL && strstr(sig->email, author_filter.c_str()) == NULL) continue;
		if (message_filter != "" && strstr(git_commit_message(c), message_filter.c_str()) == NULL) continue;
		current_entry++;
		if (current_entry < start) continue;
		inserted_entries++;
		found_commit = true;
		break;
	}
	if (!found_commit) {
		if (c) git_commit_free(c);
		return false;
	}
	if (commit) commit->release();
	commit = new nvgt_git_repository_commit(c, handle);
	return true;
}

int git_last_error_class() {
	const git_error* e = git_error_last();
	if (!e) return GIT_ERROR_NONE;
	return e->klass;
}
const std::string git_last_error_text() {
	const git_error* e = git_error_last();
	if (!e) return "";
	return std::string(e->message);
}


void RegisterGit(asIScriptEngine* engine) {
	engine->SetDefaultAccessMask(NVGT_SUBSYSTEM_GIT);
	engine->RegisterObjectType("git_repository", 0, asOBJ_REF);
	engine->RegisterFuncdef("int git_repository_match_callback(git_repository@ repo, const string&in path, const string&in user_data)");
	engine->RegisterObjectType("git_repository_entry", 0, asOBJ_REF);
	engine->RegisterObjectType("git_repository_commit", 0, asOBJ_REF);
	engine->RegisterObjectType("git_repository_commit_iterator", 0, asOBJ_REF);

	engine->RegisterObjectBehaviour("git_repository", asBEHAVE_FACTORY, "git_repository@r()", asFUNCTION(new_git_repository), asCALL_CDECL);
	engine->RegisterObjectBehaviour("git_repository", asBEHAVE_ADDREF, "void f()", asMETHOD(nvgt_git_repository, add_ref), asCALL_THISCALL);
	engine->RegisterObjectBehaviour("git_repository", asBEHAVE_RELEASE, "void f()", asMETHOD(nvgt_git_repository, release), asCALL_THISCALL);
	engine->RegisterObjectMethod("git_repository", "int open(const string&in path)", asMETHOD(nvgt_git_repository, open), asCALL_THISCALL);
	engine->RegisterObjectMethod("git_repository", "int create(const string&in path)", asMETHOD(nvgt_git_repository, create), asCALL_THISCALL);
	engine->RegisterObjectMethod("git_repository", "bool close()", asMETHOD(nvgt_git_repository, close), asCALL_THISCALL);
	engine->RegisterObjectMethod("git_repository", "int add(const string&in path)", asMETHOD(nvgt_git_repository, add), asCALL_THISCALL);
	engine->RegisterObjectMethod("git_repository", "int add_all(string[]@ paths, int flags = 0)", asMETHOD(nvgt_git_repository, add_all), asCALL_THISCALL);
	engine->RegisterObjectMethod("git_repository", "int add_all(string[]@ paths, int flags, git_repository_match_callback@ callback, const string&in callback_data = \"\")", asMETHOD(nvgt_git_repository, add_all_cb), asCALL_THISCALL);
	engine->RegisterObjectMethod("git_repository", "int update_all(string[]@ paths)", asMETHOD(nvgt_git_repository, update_all), asCALL_THISCALL);
	engine->RegisterObjectMethod("git_repository", "int update_all(string[]@ paths, git_repository_match_callback@ callback, const string&in callback_data = \"\")", asMETHOD(nvgt_git_repository, update_all_cb), asCALL_THISCALL);
	engine->RegisterObjectMethod("git_repository", "int remove(const string&in path)", asMETHOD(nvgt_git_repository, remove), asCALL_THISCALL);
	engine->RegisterObjectMethod("git_repository", "int remove_all(string[]@ paths)", asMETHOD(nvgt_git_repository, remove_all), asCALL_THISCALL);
	engine->RegisterObjectMethod("git_repository", "int remove_all(string[]@ paths, git_repository_match_callback@ callback, const string&in callback_data = \"\")", asMETHOD(nvgt_git_repository, remove_all_cb), asCALL_THISCALL);
	engine->RegisterObjectMethod("git_repository", "git_repository_commit@ commit(const string&in author, const string&in author_email, const string&in message, const string&in ref=\"HEAD\")", asMETHOD(nvgt_git_repository, commit_simple), asCALL_THISCALL);
	engine->RegisterObjectMethod("git_repository", "git_repository_commit@ commit(const string&in author, const string&in author_email, const string&in committer, const string&in committer_email, const string&in message, const string&in ref=\"HEAD\")", asMETHOD(nvgt_git_repository, commit), asCALL_THISCALL);
	engine->RegisterObjectMethod("git_repository", "string commit_diff(git_repository_commit@+ commit1, git_repository_commit@+ commit2, uint context_lines=3, uint interhunk_lines=0, uint flags=0, uint format=1, string[]@+ pathspec={}, const string&in old_prefix=\"a\", const string&in new_prefix=\"b\")", asMETHOD(nvgt_git_repository, commit_diff), asCALL_THISCALL);
	engine->RegisterObjectMethod("git_repository", "git_repository_commit@ commit_lookup(const string&in oid)", asMETHOD(nvgt_git_repository, commit_lookup), asCALL_THISCALL);
	engine->RegisterObjectMethod("git_repository", "git_repository_commit_iterator@ commit_iterate(string[]@ path_filter={}, const string&in author_filter='', const string&in message_filter='', uint64 min_time_filter=0, uint64 max_time_filter=0, uint start=0, uint count=0)", asMETHOD(nvgt_git_repository, commit_iterate), asCALL_THISCALL);
	engine->RegisterObjectMethod("git_repository", "git_repository_entry@ get_entry(uint index)", asMETHOD(nvgt_git_repository, get_entry), asCALL_THISCALL);
	engine->RegisterObjectMethod("git_repository", "int get_entry_count() property", asMETHOD(nvgt_git_repository, get_entry_count), asCALL_THISCALL);
	engine->RegisterObjectMethod("git_repository", "int get_is_empty() property", asMETHOD(nvgt_git_repository, get_is_empty), asCALL_THISCALL);
	engine->RegisterObjectMethod("git_repository", "string get_path() property", asMETHOD(nvgt_git_repository, get_path), asCALL_THISCALL);
	engine->RegisterObjectMethod("git_repository", "string get_workdir() property", asMETHOD(nvgt_git_repository, get_workdir), asCALL_THISCALL);
	engine->RegisterObjectMethod("git_repository", "bool get_active() property", asMETHOD(nvgt_git_repository, get_active), asCALL_THISCALL);

	engine->RegisterObjectBehaviour("git_repository_entry", asBEHAVE_ADDREF, "void f()", asMETHOD(nvgt_git_repository_entry, add_ref), asCALL_THISCALL);
	engine->RegisterObjectBehaviour("git_repository_entry", asBEHAVE_RELEASE, "void f()", asMETHOD(nvgt_git_repository_entry, release), asCALL_THISCALL);
	engine->RegisterObjectMethod("git_repository_entry", "uint get_ctime() property", asMETHOD(nvgt_git_repository_entry, get_ctime), asCALL_THISCALL);
	engine->RegisterObjectMethod("git_repository_entry", "uint get_mtime() property", asMETHOD(nvgt_git_repository_entry, get_mtime), asCALL_THISCALL);
	engine->RegisterObjectMethod("git_repository_entry", "uint get_file_size() property", asMETHOD(nvgt_git_repository_entry, get_file_size), asCALL_THISCALL);
	engine->RegisterObjectMethod("git_repository_entry", "string get_oid() property", asMETHOD(nvgt_git_repository_entry, get_oid), asCALL_THISCALL);
	engine->RegisterObjectMethod("git_repository_entry", "string get_path() property", asMETHOD(nvgt_git_repository_entry, get_path), asCALL_THISCALL);

	engine->RegisterObjectBehaviour("git_repository_commit", asBEHAVE_ADDREF, "void f()", asMETHOD(nvgt_git_repository_commit, add_ref), asCALL_THISCALL);
	engine->RegisterObjectBehaviour("git_repository_commit", asBEHAVE_RELEASE, "void f()", asMETHOD(nvgt_git_repository_commit, release), asCALL_THISCALL);
	engine->RegisterObjectMethod("git_repository_commit", "uint get_time() property", asMETHOD(nvgt_git_repository_commit, get_time), asCALL_THISCALL);
	engine->RegisterObjectMethod("git_repository_commit", "int get_parent_count() property", asMETHOD(nvgt_git_repository_commit, get_parent_count), asCALL_THISCALL);
	engine->RegisterObjectMethod("git_repository_commit", "git_repository_commit@ get_parent(uint)", asMETHOD(nvgt_git_repository_commit, get_parent), asCALL_THISCALL);
	engine->RegisterObjectMethod("git_repository_commit", "const string get_id() property", asMETHOD(nvgt_git_repository_commit, get_id), asCALL_THISCALL);
	engine->RegisterObjectMethod("git_repository_commit", "const string get_message() property", asMETHOD(nvgt_git_repository_commit, get_message), asCALL_THISCALL);
	engine->RegisterObjectMethod("git_repository_commit", "string get_summary() property", asMETHOD(nvgt_git_repository_commit, get_summary), asCALL_THISCALL);
	engine->RegisterObjectMethod("git_repository_commit", "const string get_body() property", asMETHOD(nvgt_git_repository_commit, get_body), asCALL_THISCALL);
	engine->RegisterObjectMethod("git_repository_commit", "const string get_raw_header() property", asMETHOD(nvgt_git_repository_commit, get_raw_header), asCALL_THISCALL);
	engine->RegisterObjectMethod("git_repository_commit", "const string get_committer() property", asMETHOD(nvgt_git_repository_commit, get_committer), asCALL_THISCALL);
	engine->RegisterObjectMethod("git_repository_commit", "const string get_committer_email() property", asMETHOD(nvgt_git_repository_commit, get_committer_email), asCALL_THISCALL);
	engine->RegisterObjectMethod("git_repository_commit", "const string get_author() property", asMETHOD(nvgt_git_repository_commit, get_author), asCALL_THISCALL);
	engine->RegisterObjectMethod("git_repository_commit", "const string get_author_email() property", asMETHOD(nvgt_git_repository_commit, get_author_email), asCALL_THISCALL);

	engine->RegisterObjectBehaviour("git_repository_commit_iterator", asBEHAVE_ADDREF, "void f()", asMETHOD(nvgt_git_repository_commit_iterator, add_ref), asCALL_THISCALL);
	engine->RegisterObjectBehaviour("git_repository_commit_iterator", asBEHAVE_RELEASE, "void f()", asMETHOD(nvgt_git_repository_commit_iterator, release), asCALL_THISCALL);
	engine->RegisterObjectMethod("git_repository_commit_iterator", "git_repository_commit@ get_commit() property", asMETHOD(nvgt_git_repository_commit_iterator, get_commit), asCALL_THISCALL);
	engine->RegisterObjectMethod("git_repository_commit_iterator", "git_repository_commit@ opImplCast()", asMETHOD(nvgt_git_repository_commit_iterator, get_commit), asCALL_THISCALL);
	engine->RegisterObjectMethod("git_repository_commit_iterator", "bool opPostInc()", asMETHOD(nvgt_git_repository_commit_iterator, next), asCALL_THISCALL);

	engine->RegisterGlobalFunction("int git_last_error_class()", asFUNCTION(git_last_error_class), asCALL_CDECL);
	engine->RegisterGlobalFunction("string git_last_error_text()", asFUNCTION(git_last_error_text), asCALL_CDECL);
}

plugin_main(nvgt_plugin_shared* shared) {
	if (!prepare_plugin(shared)) return false; // built for another plugin API version: the AngelScript functions weren't set
	// The array add-on compiled into this plugin had null memory functions (see ASAddon/src/scriptarray.cpp): letting go of the last
	// reference to an array, as add_all() does with a temporary one, crashed. The engine's are used, as unicode.cpp does.
	CScriptArray::SetMemoryFunctions(asAllocMem, asFreeMem);
	g_ScriptEngine = shared->script_engine;
	RegisterGit(shared->script_engine);
	nvgt_bundle_shared_library("git2");
	return true;
}
