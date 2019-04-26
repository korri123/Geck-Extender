#include <vector>
#include <algorithm>
#include <assert.h>
#include <CommCtrl.h>
#include "Editor.h"

bool g_UseDeferredDialogInsert;
HWND g_DeferredListView;
HWND g_DeferredComboBox;
uintptr_t g_DeferredStringLength;
bool g_AllowResize;
std::vector<std::pair<const char *, void *>> g_DeferredMenuItems;

void ResetUIDefer()
{
	g_UseDeferredDialogInsert = false;
	g_DeferredListView = nullptr;
	g_DeferredComboBox = nullptr;
	g_DeferredStringLength = 0;
	g_AllowResize = false;
	g_DeferredMenuItems.clear();
}

void BeginUIDefer()
{
	ResetUIDefer();
	g_UseDeferredDialogInsert = true;
}

void EndUIDefer()
{
	if (!g_UseDeferredDialogInsert)
		return;

	if (g_DeferredListView)
	{
		SendMessage(g_DeferredListView, WM_SETREDRAW, TRUE, 0);
		RedrawWindow(g_DeferredListView, nullptr, nullptr, RDW_ERASE | RDW_INVALIDATE | RDW_NOCHILDREN);
	}

	if (!g_DeferredMenuItems.empty())
	{
		const HWND control = g_DeferredComboBox;

		SendMessage(control, WM_SETREDRAW, FALSE, 0);// Prevent repainting until finished
		SendMessage(control, CB_SETMINVISIBLE, 1, 0);// Possible optimization for older libraries (source: MSDN forums)

													 // Sort alphabetically if requested to try and speed up inserts
		LONG_PTR style = GetWindowLongPtr(control, GWL_STYLE);

		if ((style & CBS_SORT) == CBS_SORT)
		{
			std::sort(g_DeferredMenuItems.begin(), g_DeferredMenuItems.end(),
				[](const std::pair<const char *, void *>& a, const std::pair<const char *, void *>& b) -> bool
			{
				return _stricmp(a.first, b.first) > 0;
			});
		}

		// Insert everything all at once
		SendMessage(control, CB_INITSTORAGE, g_DeferredMenuItems.size(), g_DeferredStringLength * sizeof(char));
		{
			HDC hdc = GetDC(control);
			int boxWidth = 0;

			if (!hdc) {
				_asm {int 3}
			}
//			Assert(hdc);

			for (auto& pair : g_DeferredMenuItems)
			{
				auto& display = pair.first;
				auto& value = pair.second;
				LRESULT index = SendMessageA(control, CB_ADDSTRING, 0, (LPARAM)display);
				SIZE size;

				if (index != CB_ERR && index != CB_ERRSPACE)
					SendMessageA(control, CB_SETITEMDATA, index, (LPARAM)value);

				if (g_AllowResize && GetTextExtentPoint32A(hdc, display, (int)strlen(display), &size))
					boxWidth = std::max<int>(boxWidth, size.cx);

				free((void *)display);
			}

			// Resize to fit
			if (g_AllowResize)
			{
				LRESULT currentWidth = SendMessage(control, CB_GETDROPPEDWIDTH, 0, 0);

				if (boxWidth > currentWidth)
					SendMessage(control, CB_SETDROPPEDWIDTH, boxWidth, 0);
			}

			ReleaseDC(control, hdc);
		}

		SendMessage(control, CB_SETMINVISIBLE, 30, 0);
		SendMessage(control, WM_SETREDRAW, TRUE, 0);
	}

	ResetUIDefer();
}

void InsertComboBoxItem(HWND ComboBoxHandle, const char* DisplayText, void* Value, bool AllowResize)
{
	if (!ComboBoxHandle)
		return;

	if (!DisplayText)
		DisplayText = "NONE";

	if (g_UseDeferredDialogInsert)
	{
		if (!(!g_DeferredComboBox || g_DeferredComboBox == ComboBoxHandle)) {
			_asm {int 3}
		}
		//		AssertMsg(!g_DeferredComboBox || (g_DeferredComboBox == ComboBoxHandle), "Got handles to different combo boxes? Reset probably wasn't called.");

		g_DeferredComboBox = ComboBoxHandle;
		g_DeferredStringLength += strlen(DisplayText) + 1;
		g_AllowResize |= AllowResize;

		// A copy must be created since lifetime isn't guaranteed after this function returns
		g_DeferredMenuItems.emplace_back(_strdup(DisplayText), Value);
	}
	else
	{
		if (AllowResize)
		{
			HDC hdc = GetDC(ComboBoxHandle);
			if (hdc)
			{
				SIZE size;
				if (GetTextExtentPoint32A(hdc, DisplayText, (int)strlen(DisplayText), &size))
				{
					LRESULT currentWidth = SendMessageA(ComboBoxHandle, CB_GETDROPPEDWIDTH, 0, 0);

					if (size.cx > currentWidth)
						SendMessageA(ComboBoxHandle, CB_SETDROPPEDWIDTH, size.cx, 0);
				}

				ReleaseDC(ComboBoxHandle, hdc);
			}
		}

		LRESULT index = SendMessageA(ComboBoxHandle, CB_ADDSTRING, 0, (LPARAM)DisplayText);

		if (index != CB_ERR && index != CB_ERRSPACE)
			SendMessageA(ComboBoxHandle, CB_SETITEMDATA, index, (LPARAM)Value);
	}
}

void InsertListViewItem(HWND ListViewHandle, void* Parameter, bool UseImage, int ItemIndex)
{
	if (ItemIndex == -1)
		ItemIndex = INT_MAX;

	LVITEMA item;
	memset(&item, 0, sizeof(item));

	item.mask = LVIF_PARAM | LVIF_TEXT;
	item.iItem = ItemIndex;
	item.lParam = (LPARAM)Parameter;
	item.pszText = LPSTR_TEXTCALLBACK;

	if (UseImage)
	{
		item.mask |= LVIF_IMAGE;
		item.iImage = I_IMAGECALLBACK;
	}

	if (g_UseDeferredDialogInsert)
	{
		//		AssertMsg(!g_DeferredListView || (g_DeferredListView == ListViewHandle), "Got handles to different list views? Reset probably wasn't called.");
		if (!(!g_DeferredListView || g_DeferredListView == ListViewHandle)) {
			_asm {int 3}
		}
		if (!g_DeferredListView)
		{
			g_DeferredListView = ListViewHandle;
			SendMessage(ListViewHandle, WM_SETREDRAW, FALSE, 0);
		}
	}

	ListView_InsertItem(ListViewHandle, &item);
}

void PatchTemplatedFormIterator()
{
	//
	// Add a callback that sets a global variable indicating UI dropdown menu entries can be
	// deferred to prevent redrawing/resorting after every new item insert, reducing dialog
	// initialization time.
	//
	// The _templated_ function is designed to iterate over all FORMs of a specific type - this
	// requires hooking 100-200 separate functions in the EXE as a result. False positives are
	// a non-issue as long as ctor/dtor calls are balanced.
	//
	const char* patternStr = "\xE8\x00\x00\x00\x00\x48\x89\x44\x24\x30\x48\x8B\x44\x24\x30\x48\x89\x44\x24\x38\x48\x8B\x54\x24\x38\x48\x8D\x4C\x24\x28";
	const char* maskStr = "x????xxxxxxxxxxxxxxxxxxxxxxxxx";

	UInt32 g_CodeBase = 0x401000;
	UInt32 g_CodeEnd = 0xD22500;

	for (uintptr_t i = g_CodeBase; i < g_CodeEnd;)
	{
		uintptr_t addr = XUtil::FindPattern(i, g_CodeEnd - i, (byte*)patternStr, maskStr);

		if (!addr)
			break;

		i = addr + 1;

		// Make sure the next call points to sub_14102CBEF or it's a nop from ExperimentalPatchEditAndContinue
		addr += strlen(maskStr) + 11;
		uintptr_t destination = addr + *(int*)(addr + 1) + 5;

//		if (destination != 0x14102CBEF && *(byte*)addr != 0x0F)
//			continue;

		// Now look for the matching destructor call
		uintptr_t end = 0;

		for (int j = 0; j < 2; j++)
		{
			const char* endPattern;
			const char* endMask = "x????xxx????x";

			if (j == 0)
				endPattern = "\xE8\x00\x00\x00\x00\x48\x81\xC4\x00\x00\x00\x00\xC3";// sub_140FF81CE
			else
				endPattern = "\x0F\x00\x00\x00\x00\x48\x81\xC4\x00\x00\x00\x00\xC3";// nopped version

			end = XUtil::FindPattern(addr, std::min<uintptr_t>(g_CodeEnd - addr, 1000), (byte*)endPattern, endMask);

			if (end)
				break;
		}

		if (!end)
			continue;

		// Blacklisted (000000014148C1FF): The "Use Info" dialog which has more than one list view and causes problems
		// Blacklisted (000000014169DFAD): Adding a new faction to an NPC has more than one list view

		WriteRelCall(addr, UInt32(BeginUIDefer));
		WriteRelCall(end, UInt32(EndUIDefer));
	}
}
