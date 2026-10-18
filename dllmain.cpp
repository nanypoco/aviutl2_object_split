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
	L"AviUtl無印のデフォルト挙動の分割・中間点追加コマンドを追加",
};

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

static bool replace_values(std::string& A, std::string& B) {
	const auto a = split(A);
	const auto b = split(B);

	if (a.size() < 4 || b.size() < 4) return false;

	const std::string a_target(a[a.size() - 3]);
	const std::string b_target(b[1]);

	replace(A, a.size() - 3, b_target);
	replace(B, 0, b_target);

	return true;
}

static bool replace_point(std::string& str, size_t num) {
	const auto points = split(str);
	if (points.size() < 3) return false;
	if (num + 1 >= points.size()) return false;

	replace(str, num, points[num + 1]);
	return true;
}

static void obj_split(EDIT_SECTION* edit, OBJECT_HANDLE obj, int frame){
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
		for(int i{1}; i < sec_num - 1; ++i){
			int f = edit->get_object_section_frame(obj, i);
			if (f < 0 || f > frame) break;
			edit->delete_object_section(new_obj, 1);
		}
		for (int i{sec_num - 1}; i > 0; --i) {
			int f = edit->get_object_section_frame(obj, i);
			if (f < 0 || f < frame) break;
			edit->delete_object_section(obj, i);
		}

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
		};

		for (const auto& [e, new_e] : std::ranges::zip_view(eff, new_eff)){
			Param p{e, new_e, edit, frame};
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
					p->edit->get_effect_check_value(eff, name, p->frame - 1, &val);
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
				replace_values(e_str, new_str);
				p->edit->set_effect_item_value(eff, name, e_str.c_str());
				p->edit->set_effect_item_value(new_eff, name, new_str.c_str());
				});
		}
		edit->move_object_section(new_obj, 0, frame);
		edit->move_object_section(obj, edit->get_object_section_num(obj), frame - 1);
		edit->move_object(new_obj, lf.layer, frame);
		edit->set_focus_object(new_obj);
}

static int get_frame(EDIT_SECTION* edit, bool is_click){
	if (is_click) {
		int layer{};
		int frame{};
		edit->get_mouse_layer_frame(&layer, &frame); // クリック位置
		return frame;
	}
	return edit->info->frame; // カーソル位置
}

static void add_point(EDIT_SECTION* edit, OBJECT_HANDLE obj, int frame) {
	edit->create_object_section(obj, frame);
	int sec_num = edit->get_object_section_num(obj);
	int target_sec{};
	for (int i{ 1 }; i < sec_num - 1; ++i) {
		target_sec = i;
		int f = edit->get_object_section_frame(obj, i);
		if (f < 0 || f >= frame) break;
	}
	std::vector<EFFECT_HANDLE> eff{};
	int eff_num = edit->get_effect_list(obj, nullptr, 0);
	eff.resize(eff_num);
	edit->get_effect_list(obj, eff.data(), eff_num);
	struct Param{
		EFFECT_HANDLE eff;
		EDIT_SECTION* edit;
		int target_sec;
	};
	for (auto& e : eff){
		Param p{ e, edit, target_sec };
		edit_handle->enum_effect_item(edit->get_effect_name(e), static_cast<void*>(&p), [](void* param, LPCWSTR name, int type) {
			if (type != EDIT_HANDLE::EFFECT_ITEM_TYPE_NUMBER) return; // トラックバー以外をスキップ
			auto* p = static_cast<Param*>(param);
			EFFECT_HANDLE eff = p->eff;

			TRACK_INFO info{};
			p->edit->get_effect_track_info(eff, name, &info, sizeof(TRACK_INFO));
			if (info.mode == nullptr) return; // 移動無しはスキップ

			std::string e_str = p->edit->get_effect_item_value(eff, name);
			replace_point(e_str, p->target_sec);
			p->edit->set_effect_item_value(eff, name, e_str.c_str());
			});
	}
}

using ObjectOperation = void (*)(EDIT_SECTION*, OBJECT_HANDLE, int);

struct ObjectCallbackParam {
	bool is_click;
	ObjectOperation operation;
};

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

			callback->operation(edit, obj, frame);
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
