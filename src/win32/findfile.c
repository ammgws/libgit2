/*
 * Copyright (C) the libgit2 contributors. All rights reserved.
 *
 * This file is part of libgit2, distributed under the GNU GPL v2 with
 * a Linking Exception. For full terms see the included COPYING file.
 */

#include "findfile.h"

#include "path_w32.h"
#include "utf-conv.h"
#include "path.h"

#define REG_MSYSGIT_INSTALL_LOCAL L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Uninstall\\Git_is1"

#ifndef _WIN64
#define REG_MSYSGIT_INSTALL REG_MSYSGIT_INSTALL_LOCAL
#else
#define REG_MSYSGIT_INSTALL L"SOFTWARE\\Wow6432Node\\Microsoft\\Windows\\CurrentVersion\\Uninstall\\Git_is1"
#endif

typedef struct {
	git_win32_path path;
	DWORD len;
} _findfile_path;

static int git_win32__expand_path(_findfile_path *dest, const wchar_t *src)
{
	dest->len = ExpandEnvironmentStringsW(src, dest->path, ARRAY_SIZE(dest->path));

	if (!dest->len || dest->len > ARRAY_SIZE(dest->path))
		return -1;

	return 0;
}

static int win32_path_to_8(git_str *dest, const wchar_t *src)
{
	git_win32_utf8_path utf8_path;

	if (git_win32_path_to_utf8(utf8_path, src) < 0) {
		git_error_set(GIT_ERROR_OS, "unable to convert path to UTF-8");
		return -1;
	}

	/* Convert backslashes to forward slashes */
	git_path_mkposix(utf8_path);

	return git_str_sets(dest, utf8_path);
}

static wchar_t *win32_walkpath(wchar_t *path, wchar_t *buf, size_t buflen)
{
	wchar_t term, *base = path;

	GIT_ASSERT_ARG_WITH_RETVAL(path, NULL);
	GIT_ASSERT_ARG_WITH_RETVAL(buf, NULL);
	GIT_ASSERT_ARG_WITH_RETVAL(buflen, NULL);

	term = (*path == L'"') ? *path++ : L';';

	for (buflen--; *path && *path != term && buflen; buflen--)
		*buf++ = *path++;

	*buf = L'\0'; /* reserved a byte via initial subtract */

	while (*path == term || *path == L';')
		path++;

	return (path != base) ? path : NULL;
}

static int win32_find_git_in_path(git_str *buf, const wchar_t *gitexe, const wchar_t *subdir)
{
	wchar_t *env = _wgetenv(L"PATH"), lastch;
	_findfile_path root;
	size_t gitexe_len = wcslen(gitexe);

	if (!env)
		return -1;

	while ((env = win32_walkpath(env, root.path, MAX_PATH-1)) && *root.path) {
		root.len = (DWORD)wcslen(root.path);
		lastch = root.path[root.len - 1];

		/* ensure trailing slash (MAX_PATH-1 to walkpath guarantees space) */
		if (lastch != L'/' && lastch != L'\\') {
			root.path[root.len++] = L'\\';
			root.path[root.len]   = L'\0';
		}

		if (root.len + gitexe_len >= MAX_PATH)
			continue;
		wcscpy(&root.path[root.len], gitexe);

		if (_waccess(root.path, F_OK) == 0 && root.len > 5) {
			/* replace "bin\\" or "cmd\\" with subdir */
			wcscpy(&root.path[root.len - 4], subdir);

			win32_path_to_8(buf, root.path);
			return 0;
		}
	}

	return GIT_ENOTFOUND;
}

static int win32_find_git_in_registry(
	git_str *buf, const HKEY hive, const wchar_t *key, const wchar_t *subdir)
{
	HKEY hKey;
	int error = GIT_ENOTFOUND;

	GIT_ASSERT_ARG(buf);

	if (!RegOpenKeyExW(hive, key, 0, KEY_READ, &hKey)) {
		DWORD dwType, cbData;
		git_win32_path path;

		/* Ensure that the buffer is big enough to have the suffix attached
		 * after we receive the result. */
		cbData = (DWORD)(sizeof(path) - wcslen(subdir) * sizeof(wchar_t));

		/* InstallLocation points to the root of the git directory */
		if (!RegQueryValueExW(hKey, L"InstallLocation", NULL, &dwType, (LPBYTE)path, &cbData) &&
			dwType == REG_SZ) {

			/* Append the suffix */
			wcscat(path, subdir);

			/* Convert to UTF-8, with forward slashes, and output the path
			 * to the provided buffer */
			if (!win32_path_to_8(buf, path))
				error = 0;
		}

		RegCloseKey(hKey);
	}

	return error;
}

static int win32_find_existing_dirs(
	git_str *out, const wchar_t *tmpl[])
{
	_findfile_path path16;
	git_str buf = GIT_STR_INIT;

	git_str_clear(out);

	for (; *tmpl != NULL; tmpl++) {
		if (!git_win32__expand_path(&path16, *tmpl) &&
			path16.path[0] != L'%' &&
			!_waccess(path16.path, F_OK))
		{
			win32_path_to_8(&buf, path16.path);

			if (buf.size)
				git_str_join(out, GIT_PATH_LIST_SEPARATOR, out->ptr, buf.ptr);
		}
	}

	git_str_dispose(&buf);

	return (git_str_oom(out) ? -1 : 0);
}

int git_win32__find_system_dirs(git_str *out, const wchar_t *subdir)
{
	git_str buf = GIT_STR_INIT;

	/* directories where git.exe & git.cmd are found */
	if (!win32_find_git_in_path(&buf, L"git.exe", subdir) && buf.size)
		git_str_set(out, buf.ptr, buf.size);
	else
		git_str_clear(out);

	if (!win32_find_git_in_path(&buf, L"git.cmd", subdir) && buf.size)
		git_str_join(out, GIT_PATH_LIST_SEPARATOR, out->ptr, buf.ptr);

	/* directories where git is installed according to registry */
	if (!win32_find_git_in_registry(
			&buf, HKEY_CURRENT_USER, REG_MSYSGIT_INSTALL_LOCAL, subdir) && buf.size)
		git_str_join(out, GIT_PATH_LIST_SEPARATOR, out->ptr, buf.ptr);

	if (!win32_find_git_in_registry(
			&buf, HKEY_LOCAL_MACHINE, REG_MSYSGIT_INSTALL, subdir) && buf.size)
		git_str_join(out, GIT_PATH_LIST_SEPARATOR, out->ptr, buf.ptr);

	git_str_dispose(&buf);

	return (git_str_oom(out) ? -1 : 0);
}

int git_win32__find_global_dirs(git_str *out)
{
	static const wchar_t *global_tmpls[4] = {
		L"%HOME%\\",
		L"%HOMEDRIVE%%HOMEPATH%\\",
		L"%USERPROFILE%\\",
		NULL,
	};

	return win32_find_existing_dirs(out, global_tmpls);
}

int git_win32__find_xdg_dirs(git_str *out)
{
	static const wchar_t *global_tmpls[7] = {
		L"%XDG_CONFIG_HOME%\\git",
		L"%APPDATA%\\git",
		L"%LOCALAPPDATA%\\git",
		L"%HOME%\\.config\\git",
		L"%HOMEDRIVE%%HOMEPATH%\\.config\\git",
		L"%USERPROFILE%\\.config\\git",
		NULL,
	};

	return win32_find_existing_dirs(out, global_tmpls);
}

int git_win32__find_programdata_dirs(git_str *out)
{
	static const wchar_t *programdata_tmpls[2] = {
		L"%PROGRAMDATA%\\Git",
		NULL,
	};

	return win32_find_existing_dirs(out, programdata_tmpls);
}
