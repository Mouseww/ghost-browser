#include "uia.h"

#include <windows.h>
#include <oleauto.h>
#include <uiautomation.h>

#include <algorithm>
#include <cstdio>

#include "embed.h"

namespace ghost {
namespace {

// Minimal COM owner. UIA returns raw interface pointers and every one of them
// must be released on every path, including the early returns in a recursive
// walk, so they are never held bare.
template <typename T>
class Com {
 public:
  Com() = default;
  ~Com() { reset(); }
  Com(const Com&) = delete;
  Com& operator=(const Com&) = delete;

  T** put() {
    reset();
    return &ptr_;
  }
  T* get() const { return ptr_; }
  T* operator->() const { return ptr_; }
  explicit operator bool() const { return ptr_ != nullptr; }
  T* release() {
    T* p = ptr_;
    ptr_ = nullptr;
    return p;
  }
  void reset(T* p = nullptr) {
    if (ptr_ != nullptr) ptr_->Release();
    ptr_ = p;
  }

 private:
  T* ptr_ = nullptr;
};

void ensure_com() {
  // UIA needs an apartment. The daemon serves on one thread, so initializing
  // once and leaving it is both correct and simpler than balancing counts.
  static thread_local bool initialized = false;
  if (!initialized) {
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    initialized = true;
  }
}

std::string from_bstr(BSTR value) {
  if (value == nullptr) return std::string();
  return narrow(std::wstring(value, SysStringLen(value)));
}

std::string variant_string(const VARIANT& v) {
  switch (v.vt) {
    case VT_BSTR:
      return from_bstr(v.bstrVal);
    case VT_I4: {
      char buf[32];
      std::snprintf(buf, sizeof(buf), "%ld", v.lVal);
      return std::string(buf);
    }
    case VT_R8: {
      char buf[48];
      std::snprintf(buf, sizeof(buf), "%.17g", v.dblVal);
      return std::string(buf);
    }
    case VT_BOOL:
      return v.boolVal ? "true" : "false";
    default:
      return std::string();
  }
}

bool variant_bool(const VARIANT& v, bool fallback) {
  if (v.vt == VT_BOOL) return v.boolVal != VARIANT_FALSE;
  return fallback;
}

// UIA reports a rectangle as a four-element double array holding left, top,
// width and height — not right and bottom. Reading the third and fourth entries
// as edges produces negative widths and click targets in the wrong place.
bool variant_rect(const VARIANT& v, RECT* out) {
  if ((v.vt & VT_ARRAY) == 0 || (v.vt & VT_R8) == 0) return false;
  SAFEARRAY* array = v.parray;
  if (array == nullptr || SafeArrayGetDim(array) != 1) return false;

  double* data = nullptr;
  if (FAILED(SafeArrayAccessData(array, reinterpret_cast<void**>(&data)))) return false;
  long lower = 0;
  long upper = 0;
  SafeArrayGetLBound(array, 1, &lower);
  SafeArrayGetUBound(array, 1, &upper);
  const bool ok = (upper - lower + 1) >= 4;
  if (ok) {
    out->left = static_cast<LONG>(data[0]);
    out->top = static_cast<LONG>(data[1]);
    out->right = static_cast<LONG>(data[0] + data[2]);
    out->bottom = static_cast<LONG>(data[1] + data[3]);
  }
  SafeArrayUnaccessData(array);
  return ok;
}

std::string role_name(CONTROLTYPEID type) {
  switch (type) {
    case UIA_ButtonControlTypeId: return "button";
    case UIA_CalendarControlTypeId: return "calendar";
    case UIA_CheckBoxControlTypeId: return "checkbox";
    case UIA_ComboBoxControlTypeId: return "combobox";
    case UIA_EditControlTypeId: return "edit";
    case UIA_HyperlinkControlTypeId: return "link";
    case UIA_ImageControlTypeId: return "image";
    case UIA_ListItemControlTypeId: return "listitem";
    case UIA_ListControlTypeId: return "list";
    case UIA_MenuControlTypeId: return "menu";
    case UIA_MenuBarControlTypeId: return "menubar";
    case UIA_MenuItemControlTypeId: return "menuitem";
    case UIA_ProgressBarControlTypeId: return "progressbar";
    case UIA_RadioButtonControlTypeId: return "radio";
    case UIA_ScrollBarControlTypeId: return "scrollbar";
    case UIA_SliderControlTypeId: return "slider";
    case UIA_SpinnerControlTypeId: return "spinner";
    case UIA_StatusBarControlTypeId: return "statusbar";
    case UIA_TabControlTypeId: return "tab";
    case UIA_TabItemControlTypeId: return "tabitem";
    case UIA_TextControlTypeId: return "text";
    case UIA_ToolBarControlTypeId: return "toolbar";
    case UIA_ToolTipControlTypeId: return "tooltip";
    case UIA_TreeControlTypeId: return "tree";
    case UIA_TreeItemControlTypeId: return "treeitem";
    case UIA_CustomControlTypeId: return "custom";
    case UIA_GroupControlTypeId: return "group";
    case UIA_ThumbControlTypeId: return "thumb";
    case UIA_DataGridControlTypeId: return "datagrid";
    case UIA_DataItemControlTypeId: return "dataitem";
    case UIA_DocumentControlTypeId: return "document";
    case UIA_SplitButtonControlTypeId: return "splitbutton";
    case UIA_WindowControlTypeId: return "window";
    case UIA_PaneControlTypeId: return "pane";
    case UIA_HeaderControlTypeId: return "header";
    case UIA_HeaderItemControlTypeId: return "headeritem";
    case UIA_TableControlTypeId: return "table";
    case UIA_TitleBarControlTypeId: return "titlebar";
    case UIA_SeparatorControlTypeId: return "separator";
    default: return "unknown";
  }
}

Element describe(IUIAutomationElement* node) {
  Element element;

  VARIANT v;
  VariantInit(&v);
  if (SUCCEEDED(node->GetCurrentPropertyValue(UIA_ControlTypePropertyId, &v))) {
    element.role = role_name(static_cast<CONTROLTYPEID>(v.lVal));
  }
  VariantClear(&v);

  VariantInit(&v);
  if (SUCCEEDED(node->GetCurrentPropertyValue(UIA_NamePropertyId, &v))) {
    element.name = variant_string(v);
  }
  VariantClear(&v);

  VariantInit(&v);
  if (SUCCEEDED(node->GetCurrentPropertyValue(UIA_ValueValuePropertyId, &v))) {
    element.value = variant_string(v);
  }
  VariantClear(&v);

  VariantInit(&v);
  if (SUCCEEDED(node->GetCurrentPropertyValue(UIA_AutomationIdPropertyId, &v))) {
    element.automation_id = variant_string(v);
  }
  VariantClear(&v);

  VariantInit(&v);
  if (SUCCEEDED(node->GetCurrentPropertyValue(UIA_IsEnabledPropertyId, &v))) {
    element.enabled = variant_bool(v, true);
  }
  VariantClear(&v);

  VariantInit(&v);
  if (SUCCEEDED(node->GetCurrentPropertyValue(UIA_IsOffscreenPropertyId, &v))) {
    element.offscreen = variant_bool(v, false);
  }
  VariantClear(&v);

  VariantInit(&v);
  if (SUCCEEDED(node->GetCurrentPropertyValue(UIA_HasKeyboardFocusPropertyId, &v))) {
    element.focused = variant_bool(v, false);
  }
  VariantClear(&v);

  VariantInit(&v);
  if (SUCCEEDED(node->GetCurrentPropertyValue(UIA_BoundingRectanglePropertyId, &v))) {
    variant_rect(v, &element.bounds);
  }
  VariantClear(&v);

  return element;
}

struct Walk {
  IUIAutomationTreeWalker* walker = nullptr;
  std::vector<Element>* out = nullptr;
  int max_depth = 0;
  int max_nodes = 0;
  bool keep_anonymous = true;
  std::string role;
  std::string name_contains;
};

bool wanted(const Element& element, const Walk& walk) {
  if (!walk.role.empty() && element.role != walk.role) return false;
  if (!walk.name_contains.empty() &&
      element.name.find(walk.name_contains) == std::string::npos) {
    return false;
  }
  return true;
}

void visit(IUIAutomationElement* node, int depth, Walk& walk) {
  if (static_cast<int>(walk.out->size()) >= walk.max_nodes) return;

  const Element element = describe(node);
  const bool interesting = !element.name.empty() || !element.value.empty();
  if (interesting || walk.keep_anonymous) {
    if (wanted(element, walk)) {
      Element stored = element;
      stored.depth = depth;
      stored.index = static_cast<int>(walk.out->size());
      walk.out->push_back(stored);
    }
  }

  if (depth >= walk.max_depth) return;

  Com<IUIAutomationElement> child;
  walk.walker->GetFirstChildElement(node, child.put());
  while (child) {
    visit(child.get(), depth + 1, walk);
    if (static_cast<int>(walk.out->size()) >= walk.max_nodes) return;
    Com<IUIAutomationElement> next;
    walk.walker->GetNextSiblingElement(child.get(), next.put());
    child.reset(next.release());
  }
}

// Shared entry point for both public calls.
std::vector<Element> walk_window(HWND window, int max_depth, int max_nodes,
                                 bool keep_anonymous, const std::string& role,
                                 const std::string& name_contains, std::string* error) {
  std::vector<Element> out;
  error->clear();
  if (window == nullptr || !IsWindow(window)) {
    *error = "no such window";
    return out;
  }
  if (max_depth <= 0) max_depth = 12;
  if (max_nodes <= 0) max_nodes = 2000;

  ensure_com();

  Com<IUIAutomation> automation;
  HRESULT hr = CoCreateInstance(CLSID_CUIAutomation, nullptr, CLSCTX_INPROC_SERVER,
                                IID_PPV_ARGS(automation.put()));
  if (FAILED(hr)) {
    *error = "UI Automation is unavailable (CoCreateInstance failed)";
    return out;
  }

  Com<IUIAutomationElement> root;
  hr = automation->ElementFromHandle(window, root.put());
  if (FAILED(hr) || !root) {
    *error = "UI Automation could not attach to the window";
    return out;
  }

  Com<IUIAutomationTreeWalker> walker;
  hr = automation->get_ControlViewWalker(walker.put());
  if (FAILED(hr) || !walker) {
    *error = "UI Automation has no tree walker";
    return out;
  }

  Walk state;
  state.walker = walker.get();
  state.out = &out;
  state.max_depth = max_depth;
  state.max_nodes = max_nodes;
  state.keep_anonymous = keep_anonymous;
  state.role = role;
  state.name_contains = name_contains;

  // Depth 0 is the window itself, which is never what a caller wants to click.
  Com<IUIAutomationElement> child;
  walker->GetFirstChildElement(root.get(), child.put());
  while (child) {
    visit(child.get(), 1, state);
    if (static_cast<int>(out.size()) >= max_nodes) break;
    Com<IUIAutomationElement> next;
    walker->GetNextSiblingElement(child.get(), next.put());
    child.reset(next.release());
  }
  return out;
}

}  // namespace

std::vector<Element> dump_tree(HWND window, int max_depth, int max_nodes,
                               bool keep_anonymous, std::string* error) {
  return walk_window(window, max_depth, max_nodes, keep_anonymous, std::string(),
                     std::string(), error);
}

std::vector<Element> find_elements(HWND window, const std::string& role,
                                   const std::string& name_contains, int max_depth,
                                   int max_nodes, std::string* error) {
  return walk_window(window, max_depth, max_nodes, false, role, name_contains, error);
}

bool prime_accessibility(HWND window, int timeout_ms) {
  if (window == nullptr || !IsWindow(window)) return false;
  if (timeout_ms <= 0) timeout_ms = 3000;

  // The query below is itself the trigger: asking is what makes Chromium build the
  // tree, so the first iteration is never wasted even when it finds nothing.
  const ULONGLONG deadline = GetTickCount64() + static_cast<ULONGLONG>(timeout_ms);
  std::string error;
  for (;;) {
    const std::vector<Element> nodes = dump_tree(window, 32, 800, true, &error);
    for (const Element& element : nodes) {
      // RootWebArea is the document root's automation id, and no piece of browser
      // chrome ever carries it. Matching on either signal keeps this honest even if
      // a future Chromium renames one of them.
      if (element.role == "document" || element.automation_id == "RootWebArea") {
        return true;
      }
    }
    if (GetTickCount64() >= deadline) return false;
    Sleep(120);
  }
}

bool element_center(const Element& element, int* x, int* y) {
  const int width = element.bounds.right - element.bounds.left;
  const int height = element.bounds.bottom - element.bounds.top;
  if (width <= 0 || height <= 0) return false;
  // The middle of the control: the one point every widget treats as a hit.
  *x = element.bounds.left + width / 2;
  *y = element.bounds.top + height / 2;
  return true;
}

}  // namespace ghost
