#define NOMINMAX
#include <windows.h>
#include <cstdint>

#include <plugin2.h>
#include <logger2.h>
#include <config2.h>

#include <string>
#include <vector>
#include <ranges>

EDIT_HANDLE* edit_handle;
CONFIG_HANDLE* config;
LOG_HANDLE* logger;

COMMON_PLUGIN_TABLE common_plugin_table = {
	L"object_split",
	L"AviUtl無印の「中間点追加・分割時に中間の値を反映する」がオフの時の挙動と同じ分割・中間点追加のコマンドを追加する",
};

// todo 直後ではなく近い方の値を適用、直後・直前の値を適用するコマンドを別に作る?
static std::wstring strtow(std::string_view str) {
	if (str.empty()) return std::wstring();
	int str_size = static_cast<int>(str.size());

	int size = MultiByteToWideChar(CP_UTF8, 0, str.data(), str_size, nullptr, 0);
	if (size <= 0) return std::wstring();

	std::wstring wstr(size, 0);
	MultiByteToWideChar(CP_UTF8, 0, str.data(), str_size, wstr.data(), size);
	return wstr;
}

static std::vector<std::string_view> split(const std::string& str) {
	const auto end = str.find('|');
	const std::string_view view(str.data(), end == std::string::npos ? str.size() : end);

	std::vector<std::string_view> values;
	size_t start = 0;

	while (true) {
		const auto pos = view.find(',', start);
		values.emplace_back(view.substr(start, pos == std::string_view::npos ? pos : pos - start));

		if (pos == std::string_view::npos) break;
		start = pos + 1;
	}

	return values;
}

// (元の文字列, 対象のindex, 新しい文字列)
static void replace(std::string& str, size_t index, const std::string_view value) {
	const auto end = str.find('|');
	const size_t limit = end == std::string::npos ? str.size() : end;
	size_t start = 0;

	for (size_t i = 0; i < index; ++i) {
		start = str.find(',', start) + 1;
	}

	size_t finish = str.find(',', start);
	if (finish == std::string::npos || finish >= limit) finish = limit;

	str.replace(start, finish - start, value);
};

static bool replace_values(std::string& A, std::string& B, bool replace_pre) {
	const auto a = split(A);
	const auto b = split(B);

	if (a.size() < 4 || b.size() < 4) return false;

	;
	const std::string rep(replace_pre ? a[a.size() - 4] : b[1]);

	replace(A, a.size() - 3, rep);
	replace(B, 0, rep);

	return true;
}

static bool replace_values_at_split_section(std::string& A, std::string& B) {
	const auto a = split(A);
	const auto b = split(B);

	if (a.size() < 4 || b.size() < 4) return false;

	// 対象 section は obj の最後の section
	const std::string rep(a[a.size() - 4]);

	// 対象 section の値を、削除後に obj の最後になる section にコピー
	replace(A, a.size() - 3, rep);

	// new_obj の最初の section にコピー
	replace(B, 0, rep);

	return true;
}

static bool replace_check_values_at_split_section(std::string& A, std::string& B) {
	const auto a = split(A);
	const auto b = split(B);

	if (a.size() < 2 || b.empty()) return false;

	const std::string rep(a[a.size() - 2]);

	// 対象 section の値を、削除後に obj の最後になる section にコピー
	replace(A, a.size() - 1, rep);

	// new_obj の最初の section にコピー
	replace(B, 0, rep);

	return true;
}

static void obj_split(EDIT_SECTION* edit, OBJECT_HANDLE obj, int frame, int end_frame){
		auto alias = edit->get_object_alias(obj);
		if (!alias) {
			logger->error(logger, L"オブジェクト情報の取得に失敗しました");
			return;
		}
		auto lf = edit->get_object_layer_frame(obj);
		auto new_obj = edit->create_object_from_alias(alias, edit->info->layer_max + 1, lf.start, 0);
		if (!new_obj) {
			logger->error(logger, L"オブジェクトの作成に失敗しました");
			return;
		}

		int sec_num = edit->get_object_section_num(obj);
		int pre_frame{};
		int next_frame{};
		int copy_section = -1;
		int copy_section_frame = -1;

		for (int i{ 1 }; i < sec_num; ++i) {
			int f = edit->get_object_section_frame(obj, i);
			if (f < 0 || f > frame) break;

			pre_frame = f;

			if (f == frame || (copy_section < 0 && f == frame - 1)) {
				copy_section = i;
				copy_section_frame = f;
			}

			edit->delete_object_section(new_obj, 1);
		}
		for (int i{ sec_num - 1 }; i >= 0; --i) {
			int f = edit->get_object_section_frame(obj, i);
			if (f < 0 || f < frame) break;

			next_frame = f;

			// 値のコピーが終わるまで対象 section を残す
			if (i != copy_section) edit->delete_object_section(obj, i);
		}
		if (next_frame == 0) next_frame = end_frame;

		bool replace_pre = (frame - pre_frame) < (next_frame - frame);

		std::vector<EFFECT_HANDLE> eff{};
		std::vector<EFFECT_HANDLE> new_eff{};
		int eff_num = edit->get_effect_list(obj, nullptr, 0);
		eff.resize(eff_num);
		new_eff.resize(eff_num);
		edit->get_effect_list(obj, eff.data(), eff_num);
		edit->get_effect_list(new_obj, new_eff.data(), eff_num);

		struct Param {
			EFFECT_HANDLE eff;
			EFFECT_HANDLE new_eff;
			EDIT_SECTION* edit;
			int frame;
			bool replace_pre;
			int copy_section_frame;
		};

		for (const auto& [e, new_e] : std::ranges::zip_view(eff, new_eff)){
			Param p{ e, new_e, edit, frame, replace_pre, copy_section_frame };
			edit_handle->enum_effect_item(edit->get_effect_name(e), static_cast<void*>(&p), [](void* param, LPCWSTR name, int type) {
				auto* p = static_cast<Param*>(param);
				const EFFECT_HANDLE eff = p->eff;
				const EFFECT_HANDLE new_eff = p->new_eff;
				std::string a = p->edit->get_effect_item_value(new_eff, name);
				if (type == EDIT_HANDLE::EFFECT_ITEM_TYPE_CHECK) { // チェックセクション
					// 新しいオブジェクトの先頭の値を元のオブジェクトの最後の値に置き換える
					std::string str = p->edit->get_effect_item_value(new_eff, name);
					if (str.find(',') == std::string::npos) return; // 単体ならスキップ

					bool val{};
					p->edit->get_effect_check_value(eff, name, p->frame - ((p->copy_section_frame >= 0) ? 0 : 1), &val);
					replace(str, 0, val ? "1" : "0");
					p->edit->set_effect_item_value(new_eff, name, str.c_str());
					return;
				}
				if (type != EDIT_HANDLE::EFFECT_ITEM_TYPE_NUMBER) return; // トラックバー以外をスキップ

				TRACK_INFO info{};
				p->edit->get_effect_track_info(eff, name, &info, sizeof(TRACK_INFO));
				if (info.mode == nullptr) return; // 移動無しはスキップ

				std::string e_str = p->edit->get_effect_item_value(eff, name);
				std::string new_str = p->edit->get_effect_item_value(new_eff, name);

				if (p->copy_section_frame >= 0)
					replace_values_at_split_section(e_str, new_str);
				else
					replace_values(e_str, new_str, p->replace_pre);

				p->edit->set_effect_item_value(eff, name, e_str.c_str());
				p->edit->set_effect_item_value(new_eff, name, new_str.c_str());
				});
		}
		if (copy_section >= 0) edit->delete_object_section(obj, copy_section);
		edit->move_object_section(new_obj, 0, frame);
		edit->move_object_section(obj, edit->get_object_section_num(obj), frame - 1);
		edit->move_object(new_obj, lf.layer, frame);
		edit->set_focus_object(new_obj);
}

static bool replace_point(std::string& str, size_t num, bool replace_pre) {
	const auto points = split(str);
	if (points.size() < 3) return false;

	if (replace_pre) {
		if (num == 0 || num >= points.size()) return false;
	}
	else if (num + 1 >= points.size()) return false;

	const std::string target(points[replace_pre ? num - 1 : num + 1]);
	replace(str, num, target);
	return true;
}

static void add_point(EDIT_SECTION* edit, OBJECT_HANDLE obj, int frame, int end_frame) {
	int sec_num = edit->get_object_section_num(obj);

	// 指定位置に既存の section があれば終了
	for (int i{}; i < sec_num; ++i) {
		if (edit->get_object_section_frame(obj, i) == frame) return;
	}

	edit->create_object_section(obj, frame);
	sec_num = edit->get_object_section_num(obj);

	// 追加された section のインデックスを取得
	int target_sec = -1;
	for (int i{}; i < sec_num; ++i) {
		if (edit->get_object_section_frame(obj, i) == frame) {
			target_sec = i;
			break;
		}
	}
	if (target_sec < 0) return;

	// 前後の section のフレーム
	const int prev_frame = target_sec > 0
		? edit->get_object_section_frame(obj, target_sec - 1)
		: -1;
	const int next_frame = target_sec + 1 < sec_num
		? edit->get_object_section_frame(obj, target_sec + 1)
		: end_frame;

	// 前の section の方が近ければ true。同距離なら次を選ぶ
	bool replace_pre{};
	if (prev_frame < 0) replace_pre = false;
	else if (next_frame < 0) replace_pre = true;
	else replace_pre = (frame - prev_frame) < (next_frame - frame);

	std::vector<EFFECT_HANDLE> eff{};
	int eff_num = edit->get_effect_list(obj, nullptr, 0);
	eff.resize(eff_num);
	edit->get_effect_list(obj, eff.data(), eff_num);

	struct Param {
		EFFECT_HANDLE eff;
		EDIT_SECTION* edit;
		int target_sec;
		bool replace_pre;
	};

	for (auto& e : eff) {
		Param p{ e, edit, target_sec, replace_pre };
		edit_handle->enum_effect_item(edit->get_effect_name(e), static_cast<void*>(&p), [](void* param, LPCWSTR name, int type) {
			if (type != EDIT_HANDLE::EFFECT_ITEM_TYPE_NUMBER) return; // トラックバー以外をスキップ
			auto* p = static_cast<Param*>(param);
			EFFECT_HANDLE eff = p->eff;

			TRACK_INFO info{};
			p->edit->get_effect_track_info(eff, name, &info, sizeof(TRACK_INFO));
			if (info.mode == nullptr) return; // 移動無しはスキップ

			std::string e_str = p->edit->get_effect_item_value(eff, name);
			if (!replace_point(e_str, p->target_sec, p->replace_pre)) return;

			p->edit->set_effect_item_value(eff, name, e_str.c_str());
			});
	}
}

using ObjectOperation = void (*)(EDIT_SECTION*, OBJECT_HANDLE, int, int);

struct ObjectCallbackParam {
	bool is_click;
	ObjectOperation operation;
};

static int get_frame(EDIT_SECTION* edit, bool is_click) {
	if (is_click) {
		int layer{};
		int frame{};
		edit->get_mouse_layer_frame(&layer, &frame); // クリック位置
		return frame;
	}
	return edit->info->frame; // カーソル位置
}

static void object_operation_callback(void* p) {
	edit_handle->call_edit_section_param(p, [](void* param, EDIT_SECTION* edit) {
		auto* callback = static_cast<ObjectCallbackParam*>(param);
		if (callback == nullptr || callback->operation == nullptr) return;

		const int frame = get_frame(edit, callback->is_click);
		const int select_num = edit->get_selected_object_num();

		auto apply = [&](OBJECT_HANDLE obj) {
			if (obj == nullptr) return;

			const auto lf = edit->get_object_layer_frame(obj);
			if (frame <= lf.start || frame > lf.end) return;

			callback->operation(edit, obj, frame, lf.end);
			};

		if (select_num == 0) {
			apply(edit->get_focus_object());
		}
		else {
			for (int i{}; i < select_num; ++i) {
				apply(edit->get_selected_object(i));
			}
		}
		});
}


EXTERN_C __declspec(dllexport) void InitializeConfig(CONFIG_HANDLE* handle) {
	config = handle;
}

EXTERN_C __declspec(dllexport) void InitializeLogger(LOG_HANDLE* handle) {
	logger = handle;
}

EXTERN_C __declspec(dllexport) bool InitializePlugin(DWORD version) {
	return true;
}

EXTERN_C __declspec(dllexport) void UninitializePlugin() {
}

EXTERN_C __declspec(dllexport) COMMON_PLUGIN_TABLE* GetCommonPluginTable(void) {
	return &common_plugin_table;
}

EXTERN_C __declspec(dllexport) void RegisterPlugin(HOST_APP_TABLE* host) {
	edit_handle = host->create_edit_handle();

	static ObjectCallbackParam split_cursor{ false, obj_split };
	static ObjectCallbackParam split_mouse{ true, obj_split };
	static ObjectCallbackParam point_cursor{ false, add_point };
	static ObjectCallbackParam point_mouse{ true, add_point };

	host->register_edit_menu_param(L"中間の値を保持せずオブジェクトを分割(編集カーソル位置)", &split_cursor, object_operation_callback);
	host->register_edit_menu_param(L"中間の値を保持せずオブジェクトを分割(マウスカーソル位置)", &split_mouse, object_operation_callback);
	host->register_object_menu_param(L"中間の値を保持せず分割(編集カーソル位置)", &split_cursor, object_operation_callback);
	host->register_object_menu_param(L"中間の値を保持せず分割(クリック位置)", &split_mouse, object_operation_callback);

	host->register_edit_menu_param(L"中間の値を保持せずオブジェクトに中間点を追加(編集カーソル位置)", &point_cursor, object_operation_callback);
	host->register_edit_menu_param(L"中間の値を保持せずオブジェクトに中間点を追加(マウスカーソル位置)", &point_mouse, object_operation_callback);
	host->register_object_menu_param(L"中間の値を保持せず中間点を追加(編集カーソル位置)", &point_cursor, object_operation_callback);
	host->register_object_menu_param(L"中間の値を保持せず中間点を追加(クリック位置)", &point_mouse, object_operation_callback);
}
