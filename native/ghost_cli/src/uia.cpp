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
  Com(Com&& other) noexcept : ptr_(other.release()) {}
  Com& operator=(Com&& other) noexcept {
    if (this != &other) reset(other.release());
    return *this;
  }

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

// A client that is merely *alive* is not a client that is *listening*: the flag
// Chromium reads is set when a client registers for events, not when it asks a
// question. This is the smallest event sink that can exist -- it does nothing
// with the events it receives, and exists only so that the registration, which
// is the part that matters, actually happens.
class StructureWatcher : public IUIAutomationStructureChangedEventHandler {
 public:
  HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void** out) override {
    if (out == nullptr) return E_POINTER;
    if (iid == IID_IUnknown ||
        iid == __uuidof(IUIAutomationStructureChangedEventHandler)) {
      *out = static_cast<IUIAutomationStructureChangedEventHandler*>(this);
      AddRef();
      return S_OK;
    }
    *out = nullptr;
    return E_NOINTERFACE;
  }
  ULONG STDMETHODCALLTYPE AddRef() override {
    return static_cast<ULONG>(InterlockedIncrement(&refs_));
  }
  ULONG STDMETHODCALLTYPE Release() override {
    const LONG left = InterlockedDecrement(&refs_);
    if (left == 0) delete this;
    return static_cast<ULONG>(left);
  }
  HRESULT STDMETHODCALLTYPE HandleStructureChangedEvent(IUIAutomationElement*,
                                                        StructureChangeType,
                                                        SAFEARRAY*) override {
    return S_OK;
  }

 private:
  LONG refs_ = 1;
};

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
  // When set, the walk stops on the element that would have been stored at this
  // index and hands back the live COM element instead. Indices are assigned by
  // exactly the same rule as an ordinary dump, so an index from dump_tree always
  // addresses the same control here.
  int target_index = -1;
  IUIAutomationElement* found = nullptr;  // AddRef'd
  bool stop = false;
  // How many elements this walk has stored so far, counted before the caller's
  // role/name filter. An index has to mean the same thing to find, tree and
  // click, or a caller that finds a control cannot act on it.
  int seen = 0;
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
  if (walk.stop) return;
  if (walk.seen >= walk.max_nodes) return;

  const Element element = describe(node);
  const bool interesting = !element.name.empty() || !element.value.empty();
  if (interesting || walk.keep_anonymous) {
    // The index counts every element the walk would store, matching or not: the
    // filter decides what is reported, never what a position means.
    const int index = walk.seen++;
    if (index == walk.target_index) {
      node->AddRef();
      walk.found = node;
      walk.stop = true;
      return;
    }
    if (wanted(element, walk)) {
      Element stored = element;
      stored.depth = depth;
      stored.index = index;
      walk.out->push_back(stored);
    }
  }

  if (depth >= walk.max_depth) return;

  Com<IUIAutomationElement> child;
  walk.walker->GetFirstChildElement(node, child.put());
  while (child) {
    visit(child.get(), depth + 1, walk);
    if (walk.stop) return;
    if (walk.seen >= walk.max_nodes) return;
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

// The live COM element that dump_tree would have numbered `index`. The walk
// parameters are the control plane's own, because an index is only meaningful
// against the walk that produced it.
Com<IUIAutomationElement> element_at(HWND window, int index, std::string* error) {
  Com<IUIAutomationElement> none;
  if (window == nullptr || !IsWindow(window)) {
    *error = "no such window";
    return none;
  }
  ensure_com();

  Com<IUIAutomation> automation;
  if (FAILED(CoCreateInstance(CLSID_CUIAutomation, nullptr, CLSCTX_INPROC_SERVER,
                              IID_PPV_ARGS(automation.put())))) {
    *error = "UI Automation is unavailable (CoCreateInstance failed)";
    return none;
  }

  Com<IUIAutomationElement> root;
  if (FAILED(automation->ElementFromHandle(window, root.put())) || !root) {
    *error = "UI Automation could not attach to the window";
    return none;
  }

  Com<IUIAutomationTreeWalker> walker;
  if (FAILED(automation->get_ControlViewWalker(walker.put())) || !walker) {
    *error = "UI Automation has no tree walker";
    return none;
  }

  std::vector<Element> scratch;
  Walk state;
  state.walker = walker.get();
  state.out = &scratch;
  state.max_depth = kTreeDepth;
  state.max_nodes = kTreeNodes;
  state.keep_anonymous = false;
  state.target_index = index;

  Com<IUIAutomationElement> child;
  walker->GetFirstChildElement(root.get(), child.put());
  while (child && !state.stop) {
    visit(child.get(), 1, state);
    if (state.stop) break;
    Com<IUIAutomationElement> next;
    walker->GetNextSiblingElement(child.get(), next.put());
    child.reset(next.release());
  }

  if (state.found == nullptr) {
    *error = "the control is no longer on the page";
    return none;
  }
  Com<IUIAutomationElement> found;
  found.reset(state.found);
  return found;
}

// The first pattern the control actually offers. Each is a capability the
// control advertises about itself, so asking is not a guess.
template <typename T>
bool pattern(IUIAutomationElement* element, PATTERNID id, Com<T>* out) {
  return SUCCEEDED(element->GetCurrentPatternAs(id, IID_PPV_ARGS(out->put()))) && *out;
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

// Every descendant the tree admits to having, with no view walker in the way.
//
// Chromium's control view is meant to hide nothing a caller would want, so this
// is normally the same list as dump_tree. It exists for the case where it is
// not: "the page is not exposed" and "the walker will not descend into it"
// produce the same short list, and only a different question separates them.
// Depth is deliberately left at zero -- this answers "is it there", not "where".
std::vector<Element> dump_descendants(HWND window, int max_nodes, std::string* error) {
  std::vector<Element> out;
  error->clear();
  if (window == nullptr || !IsWindow(window)) {
    *error = "no such window";
    return out;
  }
  if (max_nodes <= 0) max_nodes = 4000;

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

  Com<IUIAutomationCondition> condition;
  hr = automation->CreateTrueCondition(condition.put());
  if (FAILED(hr) || !condition) {
    *error = "UI Automation has no true condition";
    return out;
  }

  Com<IUIAutomationElementArray> found;
  hr = root->FindAll(TreeScope_Descendants, condition.get(), found.put());
  if (FAILED(hr) || !found) {
    *error = "UI Automation could not enumerate the tree";
    return out;
  }

  int count = 0;
  found->get_Length(&count);
  for (int i = 0; i < count && static_cast<int>(out.size()) < max_nodes; ++i) {
    Com<IUIAutomationElement> element;
    if (FAILED(found->GetElement(i, element.put())) || !element) continue;
    out.push_back(describe(element.get()));
  }
  return out;
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

// Holds a UI Automation client open, then reports what the platform believes.
//
// The flag this leaves behind belongs to the process, not to the query: it is
// set while a client is alive and cleared when the last one goes away. A reading
// taken inside a client that is about to exit therefore says nothing about what
// a browser saw while it was starting -- only a client that stays alive can.
// Sleeping here is the entire point of the function.
bool hold_accessibility_client(int seconds, bool* clients_listening) {
  if (clients_listening != nullptr) *clients_listening = false;
  if (seconds <= 0) seconds = 1;

  ensure_com();

  Com<IUIAutomation> automation;
  const HRESULT hr = CoCreateInstance(CLSID_CUIAutomation, nullptr, CLSCTX_INPROC_SERVER,
                                      IID_PPV_ARGS(automation.put()));
  if (FAILED(hr) || !automation) return false;

  using ListeningFn = BOOL(WINAPI*)();
  const HMODULE core = LoadLibraryW(L"uiautomationcore.dll");
  const ListeningFn listening =
      core == nullptr
          ? nullptr
          : reinterpret_cast<ListeningFn>(GetProcAddress(core, "UiaClientsAreListening"));

  // An element query registers this process as a client; an event registration
  // is what registers it as a *listener*. The desktop root is the cheapest
  // element that always exists and covers every window on the session.
  Com<IUIAutomationElement> desktop;
  automation->GetRootElement(desktop.put());

  StructureWatcher* watcher = new StructureWatcher();
  const HRESULT added = automation->AddStructureChangedEventHandler(
      desktop.get(), TreeScope_Subtree, nullptr,
      static_cast<IUIAutomationStructureChangedEventHandler*>(watcher));

  if (clients_listening != nullptr && listening != nullptr) {
    *clients_listening = listening() != FALSE;
  }

  const ULONGLONG deadline = GetTickCount64() + static_cast<ULONGLONG>(seconds) * 1000;
  while (GetTickCount64() < deadline) Sleep(100);

  if (SUCCEEDED(added)) {
    automation->RemoveStructureChangedEventHandler(
        desktop.get(),
        static_cast<IUIAutomationStructureChangedEventHandler*>(watcher));
  }
  watcher->Release();
  return true;
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

bool scroll_element_into_view(HWND window, int index, std::string* error) {
  std::string local;
  if (error == nullptr) error = &local;
  error->clear();

  Com<IUIAutomationElement> element = element_at(window, index, error);
  if (!element) return false;

  Com<IUIAutomationScrollItemPattern> scroll;
  if (!pattern(element.get(), UIA_ScrollItemPatternId, &scroll)) {
    *error = "the control does not scroll";
    return false;
  }
  if (FAILED(scroll->ScrollIntoView())) {
    *error = "the control refused to scroll";
    return false;
  }
  return true;
}

bool invoke_element(HWND window, int index, std::string* error) {
  std::string local;
  if (error == nullptr) error = &local;
  error->clear();

  Com<IUIAutomationElement> element = element_at(window, index, error);
  if (!element) return false;

  // Invoke first: it is what a button and a link advertise, and what a checkbox
  // that wants clicking exposes. Toggle and Select cover the checkboxes that
  // advertise those instead, and the legacy default action catches anything that
  // only speaks the older interface.
  Com<IUIAutomationInvokePattern> invoke;
  if (pattern(element.get(), UIA_InvokePatternId, &invoke)) {
    if (SUCCEEDED(invoke->Invoke())) return true;
    *error = "the control refused to be invoked";
    return false;
  }

  Com<IUIAutomationTogglePattern> toggle;
  if (pattern(element.get(), UIA_TogglePatternId, &toggle)) {
    if (SUCCEEDED(toggle->Toggle())) return true;
    *error = "the control refused to be toggled";
    return false;
  }

  Com<IUIAutomationSelectionItemPattern> select;
  if (pattern(element.get(), UIA_SelectionItemPatternId, &select)) {
    if (SUCCEEDED(select->Select())) return true;
    *error = "the control refused to be selected";
    return false;
  }

  Com<IUIAutomationLegacyIAccessiblePattern> legacy;
  if (pattern(element.get(), UIA_LegacyIAccessiblePatternId, &legacy)) {
    if (SUCCEEDED(legacy->DoDefaultAction())) return true;
    *error = "the control has no default action";
    return false;
  }

  *error = "the control exposes no way to be activated";
  return false;
}

bool set_element_value(HWND window, int index, const std::string& text,
                       std::string* error) {
  std::string local;
  if (error == nullptr) error = &local;
  error->clear();

  Com<IUIAutomationElement> element = element_at(window, index, error);
  if (!element) return false;

  Com<IUIAutomationValuePattern> value;
  if (!pattern(element.get(), UIA_ValuePatternId, &value)) {
    *error = "the field does not accept a value";
    return false;
  }
  BOOL read_only = FALSE;
  if (SUCCEEDED(value->get_CurrentIsReadOnly(&read_only)) && read_only != FALSE) {
    *error = "the field is read-only";
    return false;
  }

  const std::wstring wide = widen(text);
  const HRESULT hr = value->SetValue(const_cast<wchar_t*>(wide.c_str()));
  if (FAILED(hr)) {
    char message[128];
    std::snprintf(message, sizeof(message),
                  "the field refused the value (0x%08lX)",
                  static_cast<unsigned long>(hr));
    *error = message;
    return false;
  }
  return true;
}

}  // namespace ghost
