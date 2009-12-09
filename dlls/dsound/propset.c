/*  			DirectSound
 *
 * Copyright 1998 Marcus Meissner
 * Copyright 1998 Rob Riggs
 * Copyright 2000-2002 TransGaming Technologies, Inc.
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301, USA
 */

#define COBJMACROS
#include <stdarg.h>

#include "windef.h"
#include "winbase.h"
#include "winuser.h"
#include "mmsystem.h"
#include "winternl.h"
#include "winnls.h"
#include "vfwmsgs.h"
#include "mmddk.h"
#include "wine/debug.h"
#include "dsound.h"
#include "dsdriver.h"
#include "dsound_private.h"
#include "dsconf.h"

#ifdef NONAMELESSSTRUCT
# define S(x) (x).s
#else
# define S(x) (x)
#endif

WINE_DEFAULT_DEBUG_CHANNEL(dsound);

/*******************************************************************************
 *              IKsPrivatePropertySet
 */

/* IUnknown methods */
static HRESULT WINAPI IKsPrivatePropertySetImpl_QueryInterface(
    LPKSPROPERTYSET iface,
    REFIID riid,
    LPVOID *ppobj )
{
    IKsPrivatePropertySetImpl *This = (IKsPrivatePropertySetImpl *)iface;
    TRACE("(%p,%s,%p)\n",This,debugstr_guid(riid),ppobj);

    if (IsEqualIID(riid, &IID_IUnknown) ||
        IsEqualIID(riid, &IID_IKsPropertySet)) {
        *ppobj = iface;
        IUnknown_AddRef(iface);
        return S_OK;
    }
    *ppobj = NULL;
    return E_NOINTERFACE;
}

static ULONG WINAPI IKsPrivatePropertySetImpl_AddRef(LPKSPROPERTYSET iface)
{
    IKsPrivatePropertySetImpl *This = (IKsPrivatePropertySetImpl *)iface;
    ULONG ref = InterlockedIncrement(&(This->ref));
    TRACE("(%p) ref was %d\n", This, ref - 1);
    return ref;
}

static ULONG WINAPI IKsPrivatePropertySetImpl_Release(LPKSPROPERTYSET iface)
{
    IKsPrivatePropertySetImpl *This = (IKsPrivatePropertySetImpl *)iface;
    ULONG ref = InterlockedDecrement(&(This->ref));
    TRACE("(%p) ref was %d\n", This, ref + 1);

    if (!ref) {
        HeapFree(GetProcessHeap(), 0, This);
	TRACE("(%p) released\n", This);
    }
    return ref;
}

static HRESULT DSPROPERTY_WaveDeviceMappingW(
    LPVOID pPropData,
    ULONG cbPropData,
    PULONG pcbReturned )
{
    HRESULT hr = DSERR_INVALIDPARAM;
    PDSPROPERTY_DIRECTSOUNDDEVICE_WAVEDEVICEMAPPING_W_DATA ppd;
    TRACE("(pPropData=%p,cbPropData=%d,pcbReturned=%p)\n",
	  pPropData,cbPropData,pcbReturned);

    ppd = pPropData;

    if (!ppd) {
	WARN("invalid parameter: pPropData\n");
	return DSERR_INVALIDPARAM;
    }

    if (ppd->DataFlow == DIRECTSOUNDDEVICE_DATAFLOW_RENDER) {
        ULONG wod;
        unsigned int wodn;
        TRACE("DataFlow=DIRECTSOUNDDEVICE_DATAFLOW_RENDER\n");
        wodn = waveOutGetNumDevs();
        for (wod = 0; wod < wodn; wod++) {
            WAVEOUTCAPSW capsW;
            MMRESULT res;
            res = waveOutGetDevCapsW(wod, &capsW, sizeof(capsW));
            if (res == MMSYSERR_NOERROR) {
                if (lstrcmpW(capsW.szPname, ppd->DeviceName) == 0) {
                    ppd->DeviceId = DSOUND_renderer_guids[wod];
                    hr = DS_OK;
                    TRACE("found %s for %s\n", debugstr_guid(&ppd->DeviceId),
                          debugstr_w(ppd->DeviceName));
                    break;
                }
            }
        }
    } else if (ppd->DataFlow == DIRECTSOUNDDEVICE_DATAFLOW_CAPTURE) {
        ULONG wid;
        unsigned int widn;
        TRACE("DataFlow=DIRECTSOUNDDEVICE_DATAFLOW_CAPTURE\n");
        widn = waveInGetNumDevs();
        for (wid = 0; wid < widn; wid++) {
            WAVEINCAPSW capsW;
            MMRESULT res;
            res = waveInGetDevCapsW(wid, &capsW, sizeof(capsW));
            if (res == MMSYSERR_NOERROR) {
                if (lstrcmpW(capsW.szPname, ppd->DeviceName) == 0) {
                    ppd->DeviceId = DSOUND_capture_guids[wid];
                    hr = DS_OK;
                    TRACE("found %s for %s\n", debugstr_guid(&ppd->DeviceId),
                          debugstr_w(ppd->DeviceName));
                    break;
                }
            }
        }
    }

    if (pcbReturned)
        *pcbReturned = cbPropData;

    return hr;
}

static HRESULT DSPROPERTY_WaveDeviceMappingA(
    LPVOID pPropData,
    ULONG cbPropData,
    PULONG pcbReturned )
{
    DSPROPERTY_DIRECTSOUNDDEVICE_WAVEDEVICEMAPPING_A_DATA *ppd = pPropData;
    DSPROPERTY_DIRECTSOUNDDEVICE_WAVEDEVICEMAPPING_W_DATA data;
    DWORD len;
    HRESULT hr;

    TRACE("(pPropData=%p,cbPropData=%d,pcbReturned=%p)\n",
      pPropData,cbPropData,pcbReturned);

    if (!ppd || !ppd->DeviceName) {
        WARN("invalid parameter: ppd=%p\n", ppd);
        return DSERR_INVALIDPARAM;
    }

    data.DataFlow = ppd->DataFlow;
    len = MultiByteToWideChar(CP_ACP, 0, ppd->DeviceName, -1, NULL, 0);
    data.DeviceName = HeapAlloc(GetProcessHeap(), 0, len * sizeof(WCHAR));
    if (!data.DeviceName)
        return E_OUTOFMEMORY;
    MultiByteToWideChar(CP_ACP, 0, ppd->DeviceName, -1, data.DeviceName, len);

    hr = DSPROPERTY_WaveDeviceMappingW(&data, cbPropData, pcbReturned);
    HeapFree(GetProcessHeap(), 0, data.DeviceName);
    ppd->DeviceId = data.DeviceId;

    if (pcbReturned)
        *pcbReturned = cbPropData;

    return hr;
}

static HRESULT DSPROPERTY_Description1(
    LPVOID pPropData,
    ULONG cbPropData,
    PULONG pcbReturned )
{
    HRESULT err;
    GUID dev_guid;
    PDSPROPERTY_DIRECTSOUNDDEVICE_DESCRIPTION_1_DATA ppd;
    TRACE("(pPropData=%p,cbPropData=%d,pcbReturned=%p)\n",
	pPropData,cbPropData,pcbReturned);

    ppd = (PDSPROPERTY_DIRECTSOUNDDEVICE_DESCRIPTION_1_DATA) pPropData;

    if (!ppd) {
	WARN("invalid parameter: pPropData\n");
	return DSERR_INVALIDPARAM;
    }

    TRACE("DeviceId=%s\n",debugstr_guid(&ppd->DeviceId));
    if ( IsEqualGUID( &ppd->DeviceId , &GUID_NULL) ) {
	/* default device of type specified by ppd->DataFlow */
	if (ppd->DataFlow == DIRECTSOUNDDEVICE_DATAFLOW_CAPTURE) {
	    TRACE("DataFlow=DIRECTSOUNDDEVICE_DATAFLOW_CAPTURE\n");
	} else if (ppd->DataFlow == DIRECTSOUNDDEVICE_DATAFLOW_RENDER) {
	    TRACE("DataFlow=DIRECTSOUNDDEVICE_DATAFLOW_RENDER\n");
	} else {
	    TRACE("DataFlow=Unknown(%d)\n", ppd->DataFlow);
	}
	FIXME("(pPropData=%p,cbPropData=%d,pcbReturned=%p) GUID_NULL not implemented!\n",
	      pPropData,cbPropData,pcbReturned);
	return E_PROP_ID_UNSUPPORTED;
    }

    ppd->Type = DIRECTSOUNDDEVICE_TYPE_EMULATED;
    GetDeviceID(&ppd->DeviceId, &dev_guid);

    if ( IsEqualGUID( &ppd->DeviceId, &DSDEVID_DefaultPlayback) ||
	 IsEqualGUID( &ppd->DeviceId, &DSDEVID_DefaultVoicePlayback) ) {
	ULONG wod;
	unsigned int wodn;
	TRACE("DataFlow=DIRECTSOUNDDEVICE_DATAFLOW_RENDER\n");
	ppd->DataFlow = DIRECTSOUNDDEVICE_DATAFLOW_RENDER;
	wodn = waveOutGetNumDevs();
	for (wod = 0; wod < wodn; wod++) {
            if (IsEqualGUID( &dev_guid, &DSOUND_renderer_guids[wod] ) ) {
                DSDRIVERDESC desc;
                ppd->WaveDeviceId = wod;
                ppd->Devnode = wod;
                err = mmErr(waveOutMessage(UlongToHandle(wod),DRV_QUERYDSOUNDDESC,(DWORD_PTR)&desc,0));
                if (err == DS_OK) {
                    PIDSDRIVER drv = NULL;
                    lstrcpynA(ppd->DescriptionA, desc.szDesc, sizeof(ppd->DescriptionA));
                    lstrcpynA(ppd->ModuleA, desc.szDrvname, sizeof(ppd->ModuleA));
                    MultiByteToWideChar( CP_ACP, 0, desc.szDesc, -1, ppd->DescriptionW, sizeof(ppd->DescriptionW)/sizeof(WCHAR) );
                    MultiByteToWideChar( CP_ACP, 0, desc.szDrvname, -1, ppd->ModuleW, sizeof(ppd->ModuleW)/sizeof(WCHAR) );
                    err = mmErr(waveOutMessage(UlongToHandle(wod), DRV_QUERYDSOUNDIFACE, (DWORD_PTR)&drv, 0));
                    if (err == DS_OK && drv)
                        ppd->Type = DIRECTSOUNDDEVICE_TYPE_VXD;
                    else
                        WARN("waveOutMessage(DRV_QUERYDSOUNDIFACE) failed\n");
                    break;
                } else {
                    WARN("waveOutMessage(DRV_QUERYDSOUNDDESC) failed\n");
                    return E_PROP_ID_UNSUPPORTED;
                }
            }
	}
    } else if ( IsEqualGUID( &ppd->DeviceId , &DSDEVID_DefaultCapture) ||
	        IsEqualGUID( &ppd->DeviceId , &DSDEVID_DefaultVoiceCapture) ) {
	ULONG wid;
	unsigned int widn;
	TRACE("DataFlow=DIRECTSOUNDDEVICE_DATAFLOW_CAPTURE\n");
	ppd->DataFlow = DIRECTSOUNDDEVICE_DATAFLOW_CAPTURE;
	widn = waveInGetNumDevs();
	for (wid = 0; wid < widn; wid++) {
            if (IsEqualGUID( &dev_guid, &DSOUND_capture_guids[wid] ) ) {
                DSDRIVERDESC desc;
                ppd->WaveDeviceId = wid;
                ppd->Devnode = wid;
                err = mmErr(waveInMessage(UlongToHandle(wid),DRV_QUERYDSOUNDDESC,(DWORD_PTR)&desc,0));
                if (err == DS_OK) {
                    PIDSCDRIVER drv;
                    lstrcpynA(ppd->DescriptionA, desc.szDesc, sizeof(ppd->DescriptionA));
                    lstrcpynA(ppd->ModuleA, desc.szDrvname, sizeof(ppd->ModuleA));
                    MultiByteToWideChar( CP_ACP, 0, desc.szDesc, -1, ppd->DescriptionW, sizeof(ppd->DescriptionW)/sizeof(WCHAR) );
                    MultiByteToWideChar( CP_ACP, 0, desc.szDrvname, -1, ppd->ModuleW, sizeof(ppd->ModuleW)/sizeof(WCHAR) );
                    err = mmErr(waveInMessage(UlongToHandle(wid),DRV_QUERYDSOUNDIFACE,(DWORD_PTR)&drv,0));
                    if (err == DS_OK && drv)
                        ppd->Type = DIRECTSOUNDDEVICE_TYPE_VXD;
                    else
                        WARN("waveInMessage(DRV_QUERYDSOUNDIFACE) failed\n");
                    break;
                } else {
                    WARN("waveInMessage(DRV_QUERYDSOUNDDESC) failed\n");
                    return E_PROP_ID_UNSUPPORTED;
                }
            }
	}
    } else {
	BOOL found = FALSE;
	ULONG wod;
	unsigned int wodn;
	/* given specific device so try the render devices first */
	wodn = waveOutGetNumDevs();
	for (wod = 0; wod < wodn; wod++) {
            if (IsEqualGUID( &ppd->DeviceId, &DSOUND_renderer_guids[wod] ) ) {
                DSDRIVERDESC desc;
                TRACE("DataFlow=DIRECTSOUNDDEVICE_DATAFLOW_RENDER\n");
                ppd->DataFlow = DIRECTSOUNDDEVICE_DATAFLOW_RENDER;
                ppd->WaveDeviceId = wod;
                ppd->Devnode = wod;
                err = mmErr(waveOutMessage(UlongToHandle(wod),DRV_QUERYDSOUNDDESC,(DWORD_PTR)&desc,0));
                if (err == DS_OK) {
                    PIDSDRIVER drv = NULL;
                    lstrcpynA(ppd->DescriptionA, desc.szDesc, sizeof(ppd->DescriptionA));
                    lstrcpynA(ppd->ModuleA, desc.szDrvname, sizeof(ppd->ModuleA));
                    MultiByteToWideChar( CP_ACP, 0, desc.szDesc, -1, ppd->DescriptionW, sizeof(ppd->DescriptionW)/sizeof(WCHAR) );
                    MultiByteToWideChar( CP_ACP, 0, desc.szDrvname, -1, ppd->ModuleW, sizeof(ppd->ModuleW)/sizeof(WCHAR) );
                    err = mmErr(waveOutMessage(UlongToHandle(wod), DRV_QUERYDSOUNDIFACE, (DWORD_PTR)&drv, 0));
                    if (err == DS_OK && drv)
                        ppd->Type = DIRECTSOUNDDEVICE_TYPE_VXD;
                    else
                        WARN("waveOutMessage(DRV_QUERYDSOUNDIFACE) failed\n");
                    found = TRUE;
                    break;
                } else {
                    WARN("waveOutMessage(DRV_QUERYDSOUNDDESC) failed\n");
                    return E_PROP_ID_UNSUPPORTED;
                }
            }
	}

	if (found == FALSE) {
            ULONG wid;
            unsigned int widn;
            /* given specific device so try the capture devices next */
            widn = waveInGetNumDevs();
            for (wid = 0; wid < widn; wid++) {
                if (IsEqualGUID( &ppd->DeviceId, &DSOUND_capture_guids[wid] ) ) {
                    DSDRIVERDESC desc;
                    TRACE("DataFlow=DIRECTSOUNDDEVICE_DATAFLOW_CAPTURE\n");
                    ppd->DataFlow = DIRECTSOUNDDEVICE_DATAFLOW_CAPTURE;
                    ppd->WaveDeviceId = wid;
                    ppd->Devnode = wid;
                    err = mmErr(waveInMessage(UlongToHandle(wid),DRV_QUERYDSOUNDDESC,(DWORD_PTR)&desc,0));
                    if (err == DS_OK) {
                        PIDSDRIVER drv = NULL;
                        lstrcpynA(ppd->DescriptionA, desc.szDesc, sizeof(ppd->DescriptionA));
                        lstrcpynA(ppd->ModuleA, desc.szDrvname, sizeof(ppd->ModuleA));
                        MultiByteToWideChar( CP_ACP, 0, desc.szDesc, -1, ppd->DescriptionW, sizeof(ppd->DescriptionW)/sizeof(WCHAR) );
                        MultiByteToWideChar( CP_ACP, 0, desc.szDrvname, -1, ppd->ModuleW, sizeof(ppd->ModuleW)/sizeof(WCHAR) );
                        err = mmErr(waveInMessage(UlongToHandle(wid), DRV_QUERYDSOUNDIFACE, (DWORD_PTR)&drv, 0));
                        if (err == DS_OK && drv)
                            ppd->Type = DIRECTSOUNDDEVICE_TYPE_VXD;
                        else
                            WARN("waveInMessage(DRV_QUERYDSOUNDIFACE) failed\n");
                        found = TRUE;
                        break;
                    } else {
                        WARN("waveInMessage(DRV_QUERYDSOUNDDESC) failed\n");
                        return E_PROP_ID_UNSUPPORTED;
                    }
                }
            }

            if (found == FALSE) {
                WARN("device not found\n");
                return E_PROP_ID_UNSUPPORTED;
            }
	}
    }

    if (pcbReturned) {
	*pcbReturned = cbPropData;
	TRACE("*pcbReturned=%d\n", *pcbReturned);
    }

    return S_OK;
}

static HRESULT DSPROPERTY_DescriptionA(
    LPVOID pPropData,
    ULONG cbPropData,
    PULONG pcbReturned )
{
    PDSPROPERTY_DIRECTSOUNDDEVICE_DESCRIPTION_A_DATA ppd = pPropData;
    HRESULT err;
    GUID dev_guid;
    TRACE("(pPropData=%p,cbPropData=%d,pcbReturned=%p)\n",
	  pPropData,cbPropData,pcbReturned);

    TRACE("DeviceId=%s\n",debugstr_guid(&ppd->DeviceId));
    if ( IsEqualGUID( &ppd->DeviceId , &GUID_NULL) ) {
	/* default device of type specified by ppd->DataFlow */
	if (ppd->DataFlow == DIRECTSOUNDDEVICE_DATAFLOW_CAPTURE) {
	    TRACE("DataFlow=DIRECTSOUNDDEVICE_DATAFLOW_CAPTURE\n");
	} else if (ppd->DataFlow == DIRECTSOUNDDEVICE_DATAFLOW_RENDER) {
	    TRACE("DataFlow=DIRECTSOUNDDEVICE_DATAFLOW_RENDER\n");
	} else {
	    TRACE("DataFlow=Unknown(%d)\n", ppd->DataFlow);
	}
	FIXME("(pPropData=%p,cbPropData=%d,pcbReturned=%p) GUID_NULL not implemented!\n",
	      pPropData,cbPropData,pcbReturned);
	return E_PROP_ID_UNSUPPORTED;
    }

    ppd->Type = DIRECTSOUNDDEVICE_TYPE_EMULATED;
    GetDeviceID(&ppd->DeviceId, &dev_guid);

    if ( IsEqualGUID( &ppd->DeviceId , &DSDEVID_DefaultPlayback) ||
	 IsEqualGUID( &ppd->DeviceId , &DSDEVID_DefaultVoicePlayback) ) {
	ULONG wod;
	unsigned int wodn;
        if (IsEqualGUID( &ppd->DeviceId , &DSDEVID_DefaultPlayback) )
            TRACE("DSDEVID_DefaultPlayback\n");
        else
            TRACE("DSDEVID_DefaultVoicePlayback\n");
	ppd->DataFlow = DIRECTSOUNDDEVICE_DATAFLOW_RENDER;
	wodn = waveOutGetNumDevs();
	for (wod = 0; wod < wodn; wod++) {
            if (IsEqualGUID( &dev_guid, &DSOUND_renderer_guids[wod] ) ) {
                DSDRIVERDESC desc;
                ppd->WaveDeviceId = wod;
                err = mmErr(waveOutMessage(UlongToHandle(wod),DRV_QUERYDSOUNDDESC,(DWORD_PTR)&desc,0));
                if (err == DS_OK) {
                    PIDSDRIVER drv = NULL;
                    /* FIXME: this is a memory leak */
                    CHAR * szDescription = HeapAlloc(GetProcessHeap(),0,strlen(desc.szDesc) + 1);
                    CHAR * szModule = HeapAlloc(GetProcessHeap(),0,strlen(desc.szDrvname) + 1);
                    CHAR * szInterface = HeapAlloc(GetProcessHeap(),0,strlen("Interface") + 1);

                    if (szDescription && szModule && szInterface) {
                        strcpy(szDescription, desc.szDesc);
                        strcpy(szModule, desc.szDrvname);
                        strcpy(szInterface, "Interface");

                        ppd->Description = szDescription;
                        ppd->Module = szModule;
                        ppd->Interface = szInterface;
                        err = mmErr(waveOutMessage(UlongToHandle(wod), DRV_QUERYDSOUNDIFACE, (DWORD_PTR)&drv, 0));
                        if (err == DS_OK && drv)
                            ppd->Type = DIRECTSOUNDDEVICE_TYPE_VXD;
                        else
                            WARN("waveOutMessage(DRV_QUERYDSOUNDIFACE) failed\n");
                        break;
		    } else {
                        WARN("no memory\n");
                        HeapFree(GetProcessHeap(), 0, szDescription);
                        HeapFree(GetProcessHeap(), 0, szModule);
                        HeapFree(GetProcessHeap(), 0, szInterface);
                        return E_OUTOFMEMORY;
		    }
                } else {
                    WARN("waveOutMessage(DRV_QUERYDSOUNDDESC) failed\n");
                    return E_PROP_ID_UNSUPPORTED;
		}
            }
	}
    } else if (IsEqualGUID( &ppd->DeviceId , &DSDEVID_DefaultCapture) ||
	       IsEqualGUID( &ppd->DeviceId , &DSDEVID_DefaultVoiceCapture) ) {
	ULONG wid;
	unsigned int widn;
        if (IsEqualGUID( &ppd->DeviceId , &DSDEVID_DefaultCapture) )
            TRACE("DSDEVID_DefaultCapture\n");
        else
            TRACE("DSDEVID_DefaultVoiceCapture\n");
	ppd->DataFlow = DIRECTSOUNDDEVICE_DATAFLOW_CAPTURE;
	widn = waveInGetNumDevs();
	for (wid = 0; wid < widn; wid++) {
            if (IsEqualGUID( &dev_guid, &DSOUND_capture_guids[wid] ) ) {
                DSDRIVERDESC desc;
                ppd->WaveDeviceId = wid;
                err = mmErr(waveInMessage(UlongToHandle(wid),DRV_QUERYDSOUNDDESC,(DWORD_PTR)&desc,0));
                if (err == DS_OK) {
                    PIDSCDRIVER drv;
                    /* FIXME: this is a memory leak */
                    CHAR * szDescription = HeapAlloc(GetProcessHeap(),0,strlen(desc.szDesc) + 1);
                    CHAR * szModule = HeapAlloc(GetProcessHeap(),0,strlen(desc.szDrvname) + 1);
                    CHAR * szInterface = HeapAlloc(GetProcessHeap(),0,strlen("Interface") + 1);

                    if (szDescription && szModule && szInterface) {
                        strcpy(szDescription, desc.szDesc);
                        strcpy(szModule, desc.szDrvname);
                        strcpy(szInterface, "Interface");

                        ppd->Description = szDescription;
                        ppd->Module = szModule;
                        ppd->Interface = szInterface;
                        err = mmErr(waveInMessage(UlongToHandle(wid),DRV_QUERYDSOUNDIFACE,(DWORD_PTR)&drv,0));
                        if (err == DS_OK && drv)
                            ppd->Type = DIRECTSOUNDDEVICE_TYPE_VXD;
                        else
                            WARN("waveInMessage(DRV_QUERYDSOUNDIFACE) failed\n");
                        break;
		    } else {
                        WARN("no memory\n");
                        HeapFree(GetProcessHeap(), 0, szDescription);
                        HeapFree(GetProcessHeap(), 0, szModule);
                        HeapFree(GetProcessHeap(), 0, szInterface);
                        return E_OUTOFMEMORY;
		    }
                } else {
                    WARN("waveInMessage(DRV_QUERYDSOUNDDESC) failed\n");
                    return E_PROP_ID_UNSUPPORTED;
		}
            }
	}
    } else {
	BOOL found = FALSE;
	ULONG wod;
	unsigned int wodn;
	/* given specific device so try the render devices first */
        TRACE("Checking renderer devices\n");
	wodn = waveOutGetNumDevs();
	for (wod = 0; wod < wodn; wod++) {
            if (IsEqualGUID( &ppd->DeviceId, &DSOUND_renderer_guids[wod] ) ) {
                DSDRIVERDESC desc;
                TRACE("DSOUND_renderer_guids[%d]\n", wod);
                ppd->DataFlow = DIRECTSOUNDDEVICE_DATAFLOW_RENDER;
                ppd->WaveDeviceId = wod;
                err = mmErr(waveOutMessage(UlongToHandle(wod),DRV_QUERYDSOUNDDESC,(DWORD_PTR)&desc,0));
                if (err == DS_OK) {
                    PIDSDRIVER drv = NULL;
                    /* FIXME: this is a memory leak */
                    CHAR * szDescription = HeapAlloc(GetProcessHeap(),0,strlen(desc.szDesc) + 1);
                    CHAR * szModule = HeapAlloc(GetProcessHeap(),0,strlen(desc.szDrvname) + 1);
                    CHAR * szInterface = HeapAlloc(GetProcessHeap(),0,strlen("Interface") + 1);

                    if (szDescription && szModule && szInterface) {
			strcpy(szDescription, desc.szDesc);
			strcpy(szModule, desc.szDrvname);
			strcpy(szInterface, "Interface");

			ppd->Description = szDescription;
			ppd->Module = szModule;
			ppd->Interface = szInterface;
			err = mmErr(waveOutMessage(UlongToHandle(wod), DRV_QUERYDSOUNDIFACE, (DWORD_PTR)&drv, 0));
			if (err == DS_OK && drv)
				ppd->Type = DIRECTSOUNDDEVICE_TYPE_VXD;
                        else
                            WARN("waveOutMessage(DRV_QUERYDSOUNDIFACE) failed\n");
			found = TRUE;
			break;
		    } else {
                        WARN("no memory\n");
                        HeapFree(GetProcessHeap(), 0, szDescription);
                        HeapFree(GetProcessHeap(), 0, szModule);
                        HeapFree(GetProcessHeap(), 0, szInterface);
                        return E_OUTOFMEMORY;
		    }
                } else {
                    WARN("waveOutMessage(DRV_QUERYDSOUNDDESC) failed\n");
                    return E_PROP_ID_UNSUPPORTED;
		}
            }
        }

        if (found == FALSE) {
            ULONG wid;
            unsigned int widn;
            TRACE("Checking capture devices\n");
            ppd->DataFlow = DIRECTSOUNDDEVICE_DATAFLOW_CAPTURE;
            widn = waveInGetNumDevs();
            for (wid = 0; wid < widn; wid++) {
                if (IsEqualGUID( &ppd->DeviceId, &DSOUND_capture_guids[wid] ) ) {
                    DSDRIVERDESC desc;
                    TRACE("DSOUND_capture_guids[%d]\n", wid);
                    ppd->WaveDeviceId = wid;
                    err = mmErr(waveInMessage(UlongToHandle(wid),DRV_QUERYDSOUNDDESC,(DWORD_PTR)&desc,0));
                    if (err == DS_OK) {
                        PIDSCDRIVER drv;
                        /* FIXME: this is a memory leak */
                        CHAR * szDescription = HeapAlloc(GetProcessHeap(),0,strlen(desc.szDesc) + 1);
                        CHAR * szModule = HeapAlloc(GetProcessHeap(),0,strlen(desc.szDrvname) + 1);
                        CHAR * szInterface = HeapAlloc(GetProcessHeap(),0,strlen("Interface") + 1);

                        if (szDescription && szModule && szInterface) {
                            strcpy(szDescription, desc.szDesc);
                            strcpy(szModule, desc.szDrvname);
                            strcpy(szInterface, "Interface");

                            ppd->Description = szDescription;
                            ppd->Module = szModule;
                            ppd->Interface = szInterface;
                            err = mmErr(waveInMessage(UlongToHandle(wid),DRV_QUERYDSOUNDIFACE,(DWORD_PTR)&drv,0));
                            if (err == DS_OK && drv)
                                ppd->Type = DIRECTSOUNDDEVICE_TYPE_VXD;
                            else
                                WARN("waveInMessage(DRV_QUERYDSOUNDIFACE) failed\n");
                            found = TRUE;
                            break;
                        } else {
                            WARN("no memory\n");
                            HeapFree(GetProcessHeap(), 0, szDescription);
                            HeapFree(GetProcessHeap(), 0, szModule);
                            HeapFree(GetProcessHeap(), 0, szInterface);
                            return E_OUTOFMEMORY;
                        }
                    } else {
                        WARN("waveInMessage(DRV_QUERYDSOUNDDESC) failed\n");
                        return E_PROP_ID_UNSUPPORTED;
                    }
                }
            }
	}

	if (found == FALSE) {
	    WARN("device not found\n");
	    return E_PROP_ID_UNSUPPORTED;
	}
    }

    if (pcbReturned) {
	*pcbReturned = cbPropData;
	TRACE("*pcbReturned=%d\n", *pcbReturned);
    }

    return S_OK;
}

static HRESULT DSPROPERTY_DescriptionW(
    LPVOID pPropData,
    ULONG cbPropData,
    PULONG pcbReturned )
{
    PDSPROPERTY_DIRECTSOUNDDEVICE_DESCRIPTION_W_DATA ppd = pPropData;
    HRESULT err;
    GUID dev_guid;
    ULONG wod, wid, wodn, widn;
    DSDRIVERDESC desc;

    TRACE("pPropData=%p,cbPropData=%d,pcbReturned=%p)\n",
          pPropData,cbPropData,pcbReturned);

    TRACE("DeviceId=%s\n",debugstr_guid(&ppd->DeviceId));
    if ( IsEqualGUID( &ppd->DeviceId , &GUID_NULL) ) {
        /* default device of type specified by ppd->DataFlow */
        if (ppd->DataFlow == DIRECTSOUNDDEVICE_DATAFLOW_CAPTURE) {
            TRACE("DataFlow=DIRECTSOUNDDEVICE_DATAFLOW_CAPTURE\n");
            ppd->DeviceId = DSDEVID_DefaultCapture;
        } else if (ppd->DataFlow == DIRECTSOUNDDEVICE_DATAFLOW_RENDER) {
            TRACE("DataFlow=DIRECTSOUNDDEVICE_DATAFLOW_RENDER\n");
            ppd->DeviceId = DSDEVID_DefaultPlayback;
        } else {
            WARN("DataFlow=Unknown(%d)\n", ppd->DataFlow);
            return E_PROP_ID_UNSUPPORTED;
        }
    }

    GetDeviceID(&ppd->DeviceId, &dev_guid);

    wodn = waveOutGetNumDevs();
    widn = waveInGetNumDevs();
    wid = wod = dev_guid.Data4[7];
    if (!memcmp(&dev_guid, &DSOUND_renderer_guids[0], sizeof(GUID)-1)
        && wod < wodn)
    {
        ppd->DataFlow = DIRECTSOUNDDEVICE_DATAFLOW_RENDER;
        ppd->WaveDeviceId = wod;
    }
    else if (!memcmp(&dev_guid, &DSOUND_capture_guids[0], sizeof(GUID)-1)
             && wid < widn)
    {
        ppd->DataFlow = DIRECTSOUNDDEVICE_DATAFLOW_CAPTURE;
        ppd->WaveDeviceId = wid;
    }
    else
    {
        WARN("Device not found\n");
        return E_PROP_ID_UNSUPPORTED;
    }

    if (ppd->DataFlow == DIRECTSOUNDDEVICE_DATAFLOW_RENDER)
        err = waveOutMessage(UlongToHandle(wod),DRV_QUERYDSOUNDDESC,(DWORD_PTR)&desc,0);
    else
        err = waveInMessage(UlongToHandle(wod),DRV_QUERYDSOUNDDESC,(DWORD_PTR)&desc,0);

    if (err != MMSYSERR_NOERROR)
    {
        WARN("waveMessage(DRV_QUERYDSOUNDDESC) failed!\n");
        return E_PROP_ID_UNSUPPORTED;
    }
    else
    {
        /* FIXME: Still a memory leak.. */
        int desclen, modlen;
        static WCHAR wInterface[] = { 'I','n','t','e','r','f','a','c','e',0 };

        modlen = MultiByteToWideChar( CP_ACP, 0, desc.szDrvname, -1, NULL, 0 );
        desclen = MultiByteToWideChar( CP_ACP, 0, desc.szDesc, -1, NULL, 0 );
        ppd->Module = HeapAlloc(GetProcessHeap(),0,modlen*sizeof(WCHAR));
        ppd->Description = HeapAlloc(GetProcessHeap(),0,desclen*sizeof(WCHAR));
        ppd->Interface = wInterface;
        if (!ppd->Description || !ppd->Module)
        {
            WARN("Out of memory\n");
            HeapFree(GetProcessHeap(), 0, ppd->Description);
            HeapFree(GetProcessHeap(), 0, ppd->Module);
            ppd->Description = ppd->Module = NULL;
            return E_OUTOFMEMORY;
        }

        MultiByteToWideChar( CP_ACP, 0, desc.szDrvname, -1, ppd->Module, modlen );
        MultiByteToWideChar( CP_ACP, 0, desc.szDesc, -1, ppd->Description, desclen );
    }

    ppd->Type = DIRECTSOUNDDEVICE_TYPE_VXD;

    if (pcbReturned) {
        *pcbReturned = sizeof(*ppd);
        TRACE("*pcbReturned=%d\n", *pcbReturned);
    }

    return S_OK;
}

static HRESULT DSPROPERTY_Enumerate1(
    LPVOID pPropData,
    ULONG cbPropData,
    PULONG pcbReturned )
{
    PDSPROPERTY_DIRECTSOUNDDEVICE_ENUMERATE_1_DATA ppd = (PDSPROPERTY_DIRECTSOUNDDEVICE_ENUMERATE_1_DATA) pPropData;
    HRESULT err;
    TRACE("(pPropData=%p,cbPropData=%d,pcbReturned=%p)\n",
          pPropData,cbPropData,pcbReturned);

    if (ppd) {
        if (ppd->Callback) {
            unsigned devs, wod, wid;
            DSDRIVERDESC desc;
            DSPROPERTY_DIRECTSOUNDDEVICE_DESCRIPTION_1_DATA data;

            devs = waveOutGetNumDevs();
            for (wod = 0; wod < devs; ++wod) {
                err = mmErr(waveOutMessage(UlongToHandle(wod),DRV_QUERYDSOUNDDESC,(DWORD_PTR)&desc,0));
                if (err == DS_OK) {
                    PIDSCDRIVER drv;
                    ZeroMemory(&data, sizeof(data));
                    data.DataFlow = DIRECTSOUNDDEVICE_DATAFLOW_RENDER;
                    data.WaveDeviceId = wod;
                    data.DeviceId = DSOUND_renderer_guids[wod];
                    lstrcpynA(data.DescriptionA, desc.szDesc, sizeof(data.DescriptionA));
                    lstrcpynA(data.ModuleA, desc.szDrvname, sizeof(data.ModuleA));

                    MultiByteToWideChar( CP_ACP, 0, data.DescriptionA, -1, data.DescriptionW, sizeof(data.DescriptionW)/sizeof(WCHAR) );
                    MultiByteToWideChar( CP_ACP, 0, data.ModuleA, -1, data.ModuleW, sizeof(data.ModuleW)/sizeof(WCHAR) );

                    data.Type = DIRECTSOUNDDEVICE_TYPE_EMULATED;
                    err = mmErr(waveOutMessage(UlongToHandle(wod), DRV_QUERYDSOUNDIFACE, (DWORD_PTR)&drv, 0));
                    if (err == DS_OK && drv)
                        data.Type = DIRECTSOUNDDEVICE_TYPE_VXD;
                    else
                        WARN("waveOutMessage(DRV_QUERYDSOUNDIFACE) failed\n");

                    TRACE("calling Callback(%p,%p)\n", &data, ppd->Context);
                    (ppd->Callback)(&data, ppd->Context);
                }
            }

            devs = waveInGetNumDevs();
            for (wid = 0; wid < devs; ++wid) {
                err = mmErr(waveInMessage(UlongToHandle(wid),DRV_QUERYDSOUNDDESC,(DWORD_PTR)&desc,0));
                if (err == DS_OK) {
                    PIDSCDRIVER drv;
                    ZeroMemory(&data, sizeof(data));
                    data.DataFlow = DIRECTSOUNDDEVICE_DATAFLOW_CAPTURE;
                    data.WaveDeviceId = wid;
                    data.DeviceId = DSOUND_capture_guids[wid];
                    lstrcpynA(data.DescriptionA, desc.szDesc, sizeof(data.DescriptionA));
                    lstrcpynA(data.ModuleA, desc.szDrvname, sizeof(data.ModuleA));

                    MultiByteToWideChar( CP_ACP, 0, data.DescriptionA, -1, data.DescriptionW, sizeof(data.DescriptionW)/sizeof(WCHAR) );
                    MultiByteToWideChar( CP_ACP, 0, data.ModuleA, -1, data.ModuleW, sizeof(data.ModuleW)/sizeof(WCHAR) );

                    data.Type = DIRECTSOUNDDEVICE_TYPE_EMULATED;
                    err = mmErr(waveInMessage(UlongToHandle(wid), DRV_QUERYDSOUNDIFACE, (DWORD_PTR)&drv, 0));
                    if (err == DS_OK && drv)
                        data.Type = DIRECTSOUNDDEVICE_TYPE_VXD;
                    else
                        WARN("waveInMessage(DRV_QUERYDSOUNDIFACE) failed\n");

                    TRACE("calling Callback(%p,%p)\n", &data, ppd->Context);
                    (ppd->Callback)(&data, ppd->Context);
                }
            }

            return S_OK;
        }
    }

    if (pcbReturned) {
        *pcbReturned = 0;
        FIXME("*pcbReturned=%d\n", *pcbReturned);
    }

    return E_PROP_ID_UNSUPPORTED;
}

static HRESULT DSPROPERTY_EnumerateA(
    LPVOID pPropData,
    ULONG cbPropData,
    PULONG pcbReturned )
{
    PDSPROPERTY_DIRECTSOUNDDEVICE_ENUMERATE_A_DATA ppd = pPropData;
    HRESULT err;
    TRACE("(pPropData=%p,cbPropData=%d,pcbReturned=%p)\n",
          pPropData,cbPropData,pcbReturned);

    if (ppd) {
        if (ppd->Callback) {
            unsigned devs, wod, wid;
            DSDRIVERDESC desc;
            DSPROPERTY_DIRECTSOUNDDEVICE_DESCRIPTION_A_DATA data;

            devs = waveOutGetNumDevs();
            for (wod = 0; wod < devs; ++wod) {
                err = mmErr(waveOutMessage(UlongToHandle(wod),DRV_QUERYDSOUNDDESC,(DWORD_PTR)&desc,0));
                if (err == DS_OK) {
                    DWORD size;
                    err = mmErr(waveOutMessage(UlongToHandle(wod),DRV_QUERYDEVICEINTERFACESIZE,(DWORD_PTR)&size,0));
                    if (err == DS_OK) {
                        WCHAR * nameW = HeapAlloc(GetProcessHeap(),0,size);
                        if (nameW) {
                            err = mmErr(waveOutMessage(UlongToHandle(wod),DRV_QUERYDEVICEINTERFACE,(DWORD_PTR)nameW,size));
                            if (err == DS_OK) {
                                CHAR * szInterface = HeapAlloc(GetProcessHeap(),0,size/sizeof(WCHAR));
                                if (szInterface) {
                                    PIDSCDRIVER drv;
                                    ZeroMemory(&data, sizeof(data));
                                    data.DataFlow = DIRECTSOUNDDEVICE_DATAFLOW_RENDER;
                                    data.WaveDeviceId = wod;
                                    data.DeviceId = DSOUND_renderer_guids[wod];
                                    data.Description = desc.szDesc;
                                    data.Module = desc.szDrvname;
                                    WideCharToMultiByte( CP_ACP, 0, nameW, size/sizeof(WCHAR), szInterface, size/sizeof(WCHAR), NULL, NULL );
                                    data.Interface = szInterface;

                                    data.Type = DIRECTSOUNDDEVICE_TYPE_EMULATED;
                                    err = mmErr(waveOutMessage(UlongToHandle(wod), DRV_QUERYDSOUNDIFACE, (DWORD_PTR)&drv, 0));
                                    if (err == DS_OK && drv)
                                        data.Type = DIRECTSOUNDDEVICE_TYPE_VXD;
                                    else
                                        WARN("waveOutMessage(DRV_QUERYDSOUNDIFACE) failed\n");

                                    TRACE("calling Callback(%p,%p)\n", &data, ppd->Context);
                                    (ppd->Callback)(&data, ppd->Context);
                                }
                                HeapFree(GetProcessHeap(),0,szInterface);
                            }
                        }
                        HeapFree(GetProcessHeap(),0,nameW);
                    }
                }
            }

            devs = waveInGetNumDevs();
            for (wid = 0; wid < devs; ++wid) {
                err = mmErr(waveInMessage(UlongToHandle(wid),DRV_QUERYDSOUNDDESC,(DWORD_PTR)&desc,0));
                if (err == DS_OK) {
                    DWORD size;
                    err = mmErr(waveInMessage(UlongToHandle(wid),DRV_QUERYDEVICEINTERFACESIZE,(DWORD_PTR)&size,0));
                    if (err == DS_OK) {
                        WCHAR * nameW = HeapAlloc(GetProcessHeap(),0,size);
                        if (nameW) {
                            err = mmErr(waveInMessage(UlongToHandle(wid),DRV_QUERYDEVICEINTERFACE,(DWORD_PTR)nameW,size));
                            if (err == DS_OK) {
                                CHAR * szInterface = HeapAlloc(GetProcessHeap(),0,size/sizeof(WCHAR));
                                if (szInterface) {
                                    PIDSCDRIVER drv;
                                    ZeroMemory(&data, sizeof(data));
                                    data.DataFlow = DIRECTSOUNDDEVICE_DATAFLOW_CAPTURE;
                                    data.WaveDeviceId = wid;
                                    data.DeviceId = DSOUND_capture_guids[wid];
                                    data.Description = desc.szDesc;
                                    data.Module = desc.szDrvname;
                                    WideCharToMultiByte( CP_ACP, 0, nameW, size/sizeof(WCHAR), szInterface, size/sizeof(WCHAR), NULL, NULL );
                                    data.Interface = szInterface;

                                    data.Type = DIRECTSOUNDDEVICE_TYPE_EMULATED;
                                    err = mmErr(waveInMessage(UlongToHandle(wid), DRV_QUERYDSOUNDIFACE, (DWORD_PTR)&drv, 0));
                                    if (err == DS_OK && drv)
                                        data.Type = DIRECTSOUNDDEVICE_TYPE_VXD;
                                    else
                                        WARN("waveInMessage(DRV_QUERYDSOUNDIFACE) failed\n");

                                    TRACE("calling Callback(%p,%p)\n", &data, ppd->Context);
                                    (ppd->Callback)(&data, ppd->Context);
                                }
                                HeapFree(GetProcessHeap(),0,szInterface);
                            }
                        }
                        HeapFree(GetProcessHeap(),0,nameW);
                    }
                }
            }

            return S_OK;
        }
    }

    if (pcbReturned) {
        *pcbReturned = 0;
        FIXME("*pcbReturned=%d\n", *pcbReturned);
    }

    return E_PROP_ID_UNSUPPORTED;
}

static HRESULT DSPROPERTY_EnumerateW(
    LPVOID pPropData,
    ULONG cbPropData,
    PULONG pcbReturned )
{
    PDSPROPERTY_DIRECTSOUNDDEVICE_ENUMERATE_W_DATA ppd = pPropData;
    DSPROPERTY_DIRECTSOUNDDEVICE_DESCRIPTION_W_DATA data;
    BOOL ret;
    int widn, wodn, i;
    TRACE("(pPropData=%p,cbPropData=%d,pcbReturned=%p)\n",
          pPropData,cbPropData,pcbReturned);

    if (pcbReturned)
        *pcbReturned = 0;

    if (!ppd || !ppd->Callback)
    {
        WARN("Invalid ppd %p\n", ppd);
        return E_PROP_ID_UNSUPPORTED;
    }

    wodn = waveOutGetNumDevs();
    widn = waveInGetNumDevs();

    data.DeviceId = DSOUND_renderer_guids[0];
    for (i = 0; i < wodn; ++i)
    {
        HRESULT hr;
        data.DeviceId.Data4[7] = i;
        hr = DSPROPERTY_DescriptionW(&data, sizeof(data), NULL);
        if (FAILED(hr))
        {
            ERR("DescriptionW failed!\n");
            return S_OK;
        }
        ret = ppd->Callback(&data, ppd->Context);
        HeapFree(GetProcessHeap(), 0, data.Module);
        HeapFree(GetProcessHeap(), 0, data.Description);
        if (!ret)
            return S_OK;
    }

    data.DeviceId = DSOUND_capture_guids[0];
    for (i = 0; i < widn; ++i)
    {
        HRESULT hr;
        data.DeviceId.Data4[7] = i;
        hr = DSPROPERTY_DescriptionW(&data, sizeof(data), NULL);
        if (FAILED(hr))
        {
            ERR("DescriptionW failed!\n");
            return S_OK;
        }
        ret = ppd->Callback(&data, ppd->Context);
        HeapFree(GetProcessHeap(), 0, data.Module);
        HeapFree(GetProcessHeap(), 0, data.Description);
        if (!ret)
            return S_OK;
    }
    return S_OK;
}

static HRESULT WINAPI IKsPrivatePropertySetImpl_Get(
    LPKSPROPERTYSET iface,
    REFGUID guidPropSet,
    ULONG dwPropID,
    LPVOID pInstanceData,
    ULONG cbInstanceData,
    LPVOID pPropData,
    ULONG cbPropData,
    PULONG pcbReturned )
{
    IKsPrivatePropertySetImpl *This = (IKsPrivatePropertySetImpl *)iface;
    TRACE("(iface=%p,guidPropSet=%s,dwPropID=%d,pInstanceData=%p,cbInstanceData=%d,pPropData=%p,cbPropData=%d,pcbReturned=%p)\n",
          This,debugstr_guid(guidPropSet),dwPropID,pInstanceData,cbInstanceData,pPropData,cbPropData,pcbReturned);

    if ( IsEqualGUID( &DSPROPSETID_DirectSoundDevice, guidPropSet) ) {
        switch (dwPropID) {
        case DSPROPERTY_DIRECTSOUNDDEVICE_WAVEDEVICEMAPPING_A:
            return DSPROPERTY_WaveDeviceMappingA(pPropData,cbPropData,pcbReturned);
        case DSPROPERTY_DIRECTSOUNDDEVICE_DESCRIPTION_1:
            return DSPROPERTY_Description1(pPropData,cbPropData,pcbReturned);
        case DSPROPERTY_DIRECTSOUNDDEVICE_ENUMERATE_1:
            return DSPROPERTY_Enumerate1(pPropData,cbPropData,pcbReturned);
        case DSPROPERTY_DIRECTSOUNDDEVICE_WAVEDEVICEMAPPING_W:
            return DSPROPERTY_WaveDeviceMappingW(pPropData,cbPropData,pcbReturned);
        case DSPROPERTY_DIRECTSOUNDDEVICE_DESCRIPTION_A:
            return DSPROPERTY_DescriptionA(pPropData,cbPropData,pcbReturned);
        case DSPROPERTY_DIRECTSOUNDDEVICE_DESCRIPTION_W:
            return DSPROPERTY_DescriptionW(pPropData,cbPropData,pcbReturned);
        case DSPROPERTY_DIRECTSOUNDDEVICE_ENUMERATE_A:
            return DSPROPERTY_EnumerateA(pPropData,cbPropData,pcbReturned);
        case DSPROPERTY_DIRECTSOUNDDEVICE_ENUMERATE_W:
            return DSPROPERTY_EnumerateW(pPropData,cbPropData,pcbReturned);
        default:
            FIXME("unsupported ID: %d\n",dwPropID);
            break;
        }
    } else {
        FIXME("unsupported property: %s\n",debugstr_guid(guidPropSet));
    }

    if (pcbReturned) {
        *pcbReturned = 0;
        FIXME("*pcbReturned=%d\n", *pcbReturned);
    }

    return E_PROP_ID_UNSUPPORTED;
}

static HRESULT WINAPI IKsPrivatePropertySetImpl_Set(
    LPKSPROPERTYSET iface,
    REFGUID guidPropSet,
    ULONG dwPropID,
    LPVOID pInstanceData,
    ULONG cbInstanceData,
    LPVOID pPropData,
    ULONG cbPropData )
{
    IKsPrivatePropertySetImpl *This = (IKsPrivatePropertySetImpl *)iface;

    FIXME("(%p,%s,%d,%p,%d,%p,%d), stub!\n",This,debugstr_guid(guidPropSet),dwPropID,pInstanceData,cbInstanceData,pPropData,cbPropData);
    return E_PROP_ID_UNSUPPORTED;
}

static HRESULT WINAPI IKsPrivatePropertySetImpl_QuerySupport(
    LPKSPROPERTYSET iface,
    REFGUID guidPropSet,
    ULONG dwPropID,
    PULONG pTypeSupport )
{
    IKsPrivatePropertySetImpl *This = (IKsPrivatePropertySetImpl *)iface;
    TRACE("(%p,%s,%d,%p)\n",This,debugstr_guid(guidPropSet),dwPropID,pTypeSupport);

    if ( IsEqualGUID( &DSPROPSETID_DirectSoundDevice, guidPropSet) ) {
	switch (dwPropID) {
	case DSPROPERTY_DIRECTSOUNDDEVICE_WAVEDEVICEMAPPING_A:
	    *pTypeSupport = KSPROPERTY_SUPPORT_GET;
	    return S_OK;
	case DSPROPERTY_DIRECTSOUNDDEVICE_DESCRIPTION_1:
	    *pTypeSupport = KSPROPERTY_SUPPORT_GET;
	    return S_OK;
	case DSPROPERTY_DIRECTSOUNDDEVICE_ENUMERATE_1:
	    *pTypeSupport = KSPROPERTY_SUPPORT_GET;
	    return S_OK;
	case DSPROPERTY_DIRECTSOUNDDEVICE_WAVEDEVICEMAPPING_W:
	    *pTypeSupport = KSPROPERTY_SUPPORT_GET;
	    return S_OK;
	case DSPROPERTY_DIRECTSOUNDDEVICE_DESCRIPTION_A:
	    *pTypeSupport = KSPROPERTY_SUPPORT_GET;
	    return S_OK;
	case DSPROPERTY_DIRECTSOUNDDEVICE_DESCRIPTION_W:
	    *pTypeSupport = KSPROPERTY_SUPPORT_GET;
	    return S_OK;
	case DSPROPERTY_DIRECTSOUNDDEVICE_ENUMERATE_A:
	    *pTypeSupport = KSPROPERTY_SUPPORT_GET;
	    return S_OK;
	case DSPROPERTY_DIRECTSOUNDDEVICE_ENUMERATE_W:
	    *pTypeSupport = KSPROPERTY_SUPPORT_GET;
	    return S_OK;
	default:
            FIXME("unsupported ID: %d\n",dwPropID);
	    break;
	}
    } else {
	FIXME("unsupported property: %s\n",debugstr_guid(guidPropSet));
    }

    return E_PROP_ID_UNSUPPORTED;
}

static const IKsPropertySetVtbl ikspvt = {
    IKsPrivatePropertySetImpl_QueryInterface,
    IKsPrivatePropertySetImpl_AddRef,
    IKsPrivatePropertySetImpl_Release,
    IKsPrivatePropertySetImpl_Get,
    IKsPrivatePropertySetImpl_Set,
    IKsPrivatePropertySetImpl_QuerySupport
};

HRESULT IKsPrivatePropertySetImpl_Create(
    REFIID riid,
    IKsPrivatePropertySetImpl **piks)
{
    IKsPrivatePropertySetImpl *iks;
    TRACE("(%s, %p)\n", debugstr_guid(riid), piks);

    if (!IsEqualIID(riid, &IID_IUnknown) &&
        !IsEqualIID(riid, &IID_IKsPropertySet)) {
        *piks = 0;
        return E_NOINTERFACE;
    }

    iks = HeapAlloc(GetProcessHeap(),0,sizeof(*iks));
    iks->ref = 1;
    iks->lpVtbl = &ikspvt;

    *piks = iks;
    return S_OK;
}
