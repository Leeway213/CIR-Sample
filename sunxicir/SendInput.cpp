/** @file
*
* MIT License

* Copyright (c) 2017 Leeway213

* Permission is hereby granted, free of charge, to any person obtaining a copy
* of this software and associated documentation files (the "Software"), to deal
* in the Software without restriction, including without limitation the rights
* to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
* copies of the Software, and to permit persons to whom the Software is
* furnished to do so, subject to the following conditions:

* The above copyright notice and this permission notice shall be included in all
* copies or substantial portions of the Software.

* THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
* IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
* FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
* AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
* LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
* OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
* SOFTWARE.
*
*
**/

#include "HidInject.h"
#include "SendInput.h"
#include "Trace.h"

HIDINJECTOR_INPUT_REPORT KeyboardState = { 0 };
HIDINJECTOR_INPUT_REPORT MouseState = { 0 };

NTSTATUS SendHidReport(HIDINJECTOR_INPUT_REPORT * Report, PDEVICE_CONTEXT pDevContext)
{
	NTSTATUS status = STATUS_SUCCESS;
	PUCHAR buffer;
	ULONG bufferLength;
	ULONG_PTR bufferWrittenLen;
	WDF_MEMORY_DESCRIPTOR memoryDescriptor;
	WDFMEMORY memWrite = NULL;

	if (pDevContext->HidInjectorIoTarget == NULL)
	{
		DbgPrint_E("Io target is invalid\n");
		goto Exit;
	}

	bufferLength = sizeof(*Report);

	status = WdfMemoryCreate(WDF_NO_OBJECT_ATTRIBUTES, NonPagedPool, 0, bufferLength, &memWrite, (PVOID*)&buffer);
	if (!NT_SUCCESS(status))
	{
		DbgPrint_E("failed to create memory 0x%x\n", status);
		goto Exit;
	}

	if (Report != NULL)
	{
		RtlCopyMemory(buffer, Report, sizeof(*Report));
	}

	WDF_MEMORY_DESCRIPTOR_INIT_HANDLE(&memoryDescriptor, memWrite, NULL);
	UINT i = 0;
	for (i = 0; i < 2; i++)
	{
		status = WdfIoTargetSendWriteSynchronously(
			pDevContext->HidInjectorIoTarget,
			NULL,
			&memoryDescriptor,
			NULL,
			NULL,
			&bufferWrittenLen);

		if (!NT_SUCCESS(status))
		{
			if (i == 0)
			{
				DbgPrint_E("Failed to send write request 0x%x and will retry\n", status);
			}
			else
			{
				DbgPrint_E("Failed to send write request 0x%x\n", status);
			}
			continue;
		}
		else
		{
			break;
		}
	}

Exit:
	if (memWrite != NULL)
	{
		WdfObjectDelete(memWrite);
		memWrite = NULL;
	}

	return status;
}

UCHAR GetUsage(LPINPUT Input)
{
	if (Input->ki.dwFlags & KEYEVENTF_EXTENDEDKEY)
	{
		// Can't handle these types of events
		return USAGE_NONE;
	}
	if (Input->ki.dwFlags & KEYEVENTF_UNICODE)
	{
		return UnicodeToKeyboardUsage(Input->ki.wScan);
	}
	else if (Input->ki.dwFlags & KEYEVENTF_SCANCODE)
	{
		return ScanCodeToKeyboardUsage((UCHAR)Input->ki.wScan);
	}
	else
	{
		return VKeyToKeyboardUsage((UCHAR)Input->ki.wVk);
	}

}

BOOL InjectKeyboardSingle(LPINPUT Input, PDEVICE_CONTEXT pDevContext)
{
	BOOL ret = FALSE;
	KeyboardState.ReportId = KEYBOARD_REPORT_ID;	// TODO: this needs a better location
	UCHAR usage = GetUsage(Input);

	if (usage != USAGE_NONE)
	{
		BOOL sendDown = FALSE;
		BOOL sendUp = FALSE;

		// For unicode events, we get a single call for down and up.  
		// For vkey and scancode, we get individual down and up calls.
		if (Input->ki.dwFlags & KEYEVENTF_UNICODE)
		{
			sendDown = TRUE;
			sendUp = TRUE;
		}
		else if (Input->ki.dwFlags & KEYEVENTF_KEYUP)
		{
			sendUp = TRUE;
		}
		else
		{
			sendDown = TRUE;
		}

		if (sendDown)
		{
			if (SetKeybaordUsage(&KeyboardState, usage) &&
				NT_SUCCESS(SendHidReport(&KeyboardState, pDevContext)))
			{
				ret = TRUE;
			}
		}

		if (sendUp)
		{
			if (ClearKeyboardUsage(&KeyboardState, usage) &&
				NT_SUCCESS(SendHidReport(&KeyboardState, pDevContext)))
			{
				ret = TRUE;
			}
		}
	}

	return ret;
}

BOOL InjectMouseSingle(LPINPUT Input, PDEVICE_CONTEXT pDevContext)
{
	MouseState.ReportId = MOUSE_REPORT_ID;	// TODO: this needs a better location

											// Can't do relative.  
	if (Input->mi.dwFlags & MOUSEEVENTF_ABSOLUTE == 0)
	{
		return FALSE;
	}

	if (Input->mi.dwFlags & MOUSEEVENTF_LEFTDOWN)
	{
		MouseState.Report.MouseReport.Buttons |= MOUSE_BUTTON_1;
	}
	if (Input->mi.dwFlags & MOUSEEVENTF_LEFTUP)
	{
		MouseState.Report.MouseReport.Buttons &= ~MOUSE_BUTTON_1;
	}
	if (Input->mi.dwFlags & MOUSEEVENTF_RIGHTDOWN)
	{
		MouseState.Report.MouseReport.Buttons |= MOUSE_BUTTON_2;
	}
	if (Input->mi.dwFlags & MOUSEEVENTF_RIGHTUP)
	{
		MouseState.Report.MouseReport.Buttons &= ~MOUSE_BUTTON_2;
	}

	// API accepts 0-65535, but driver expects 0-32767.  
	MouseState.Report.MouseReport.AbsoluteX = Input->mi.dx >> 1;
	MouseState.Report.MouseReport.AbsoluteY = Input->mi.dy >> 1;

	return NT_SUCCESS(SendHidReport(&MouseState, pDevContext));
}

BOOL InjectSendInputSingle(LPINPUT Input, PDEVICE_CONTEXT pDevContext)
{

	if (Input->type == INPUT_KEYBOARD)
	{
		return InjectKeyboardSingle(Input, pDevContext);
	}
	else if (Input->type == INPUT_MOUSE)
	{
		return InjectMouseSingle(Input, pDevContext);
	}
	else
	{
		return FALSE;
	}
}


UINT InjectSendInput(
	_In_ UINT    nInputs,
	_In_ LPINPUT pInputs,
	_In_ int     cbSize,
	PDEVICE_CONTEXT pDevContext
)
{
	UINT c = 0;
	
	UNREFERENCED_PARAMETER(cbSize);

	for (UINT i = 0; i < nInputs; i++)
	{
		if (InjectSendInputSingle(&pInputs[i], pDevContext))
		{
			c++;
		}
	}
	return 0;
}

void InjectKeyDown(UCHAR vk, PDEVICE_CONTEXT pDevContext)
{
	INPUT inp = { 0 };
	inp.type = INPUT_KEYBOARD;
	inp.ki.wVk = vk;
	InjectSendInput(1, &inp, sizeof(inp), pDevContext);
}

void InjectKeyUp(UCHAR vk, PDEVICE_CONTEXT pDevContext)
{
	INPUT inp = { 0 };
	inp.type = INPUT_KEYBOARD;
	inp.ki.dwFlags = KEYEVENTF_KEYUP;
	inp.ki.wVk = vk;
	InjectSendInput(1, &inp, sizeof(inp), pDevContext);

}

void InjectScanKeyDown(
	WORD scanCode,
	PDEVICE_CONTEXT pDevContext
)
{
	INPUT inp = { 0 };
	inp.type = INPUT_KEYBOARD;
	inp.ki.dwFlags = KEYEVENTF_SCANCODE;
	inp.ki.wScan = scanCode;
	InjectSendInput(1, &inp, sizeof(inp),pDevContext);
}

void InjectUnicode(
	WORD wch,
	PDEVICE_CONTEXT pDevContext
)
{
	INPUT inp = { 0 };
	inp.type = INPUT_KEYBOARD;
	inp.ki.dwFlags = KEYEVENTF_UNICODE;
	inp.ki.wScan = wch;
	InjectSendInput(1, &inp, sizeof(inp), pDevContext);
}

void InjectScanKeyUp(
	WORD scanCode,
	PDEVICE_CONTEXT pDevContext
)
{
	INPUT inp = { 0 };
	inp.type = INPUT_KEYBOARD;
	inp.ki.dwFlags = KEYEVENTF_SCANCODE | KEYEVENTF_KEYUP;
	inp.ki.wScan = scanCode;
	InjectSendInput(1, &inp, sizeof(inp), pDevContext);
}

void InjectMouseMove(
	WORD X,
	WORD Y,
	UINT Buttons,
	PDEVICE_CONTEXT pDevContext
)
{
	INPUT inp = { 0 };
	inp.type = INPUT_MOUSE;
	inp.mi.dwFlags = MOUSEEVENTF_ABSOLUTE | Buttons;
	inp.mi.dx = X;
	inp.mi.dy = Y;

	InjectSendInput(1, &inp, sizeof(inp), pDevContext);
}