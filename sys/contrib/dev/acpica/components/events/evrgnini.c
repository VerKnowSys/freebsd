/******************************************************************************
 *
 * Module Name: evrgnini- ACPI AddressSpace (OpRegion) init
 *
 *****************************************************************************/

/*
 * Copyright (C) 2000 - 2012, Intel Corp.
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions, and the following disclaimer,
 *    without modification.
 * 2. Redistributions in binary form must reproduce at minimum a disclaimer
 *    substantially similar to the "NO WARRANTY" disclaimer below
 *    ("Disclaimer") and any redistribution must be conditioned upon
 *    including a substantially similar Disclaimer requirement for further
 *    binary redistribution.
 * 3. Neither the names of the above-listed copyright holders nor the names
 *    of any contributors may be used to endorse or promote products derived
 *    from this software without specific prior written permission.
 *
 * Alternatively, this software may be distributed under the terms of the
 * GNU General Public License ("GPL") version 2 as published by the Free
 * Software Foundation.
 *
 * NO WARRANTY
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 * "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 * LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTIBILITY AND FITNESS FOR
 * A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
 * HOLDERS OR CONTRIBUTORS BE LIABLE FOR SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS
 * OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT,
 * STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING
 * IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 * POSSIBILITY OF SUCH DAMAGES.
 */


#define __EVRGNINI_C__

#include <contrib/dev/acpica/include/acpi.h>
#include <contrib/dev/acpica/include/accommon.h>
#include <contrib/dev/acpica/include/acevents.h>
#include <contrib/dev/acpica/include/acnamesp.h>

#define _COMPONENT          ACPI_EVENTS
        ACPI_MODULE_NAME    ("evrgnini")

/* Local prototypes */

static BOOLEAN
AcpiEvIsPciRootBridge (
    ACPI_NAMESPACE_NODE     *Node);


/*******************************************************************************
 *
 * FUNCTION:    AcpiEvSystemMemoryRegionSetup
 *
 * PARAMETERS:  Handle              - Region we are interested in
 *              Function            - Start or stop
 *              HandlerContext      - Address space handler context
 *              RegionContext       - Region specific context
 *
 * RETURN:      Status
 *
 * DESCRIPTION: Setup a SystemMemory operation region
 *
 ******************************************************************************/

ACPI_STATUS
AcpiEvSystemMemoryRegionSetup (
    ACPI_HANDLE             Handle,
    UINT32                  Function,
    void                    *HandlerContext,
    void                    **RegionContext)
{
    ACPI_OPERAND_OBJECT     *RegionDesc = (ACPI_OPERAND_OBJECT *) Handle;
    ACPI_MEM_SPACE_CONTEXT  *LocalRegionContext;


    ACPI_FUNCTION_TRACE (EvSystemMemoryRegionSetup);


    if (Function == ACPI_REGION_DEACTIVATE)
    {
        if (*RegionContext)
        {
            LocalRegionContext = (ACPI_MEM_SPACE_CONTEXT *) *RegionContext;

            /* Delete a cached mapping if present */

            if (LocalRegionContext->MappedLength)
            {
                AcpiOsUnmapMemory (LocalRegionContext->MappedLogicalAddress,
                    LocalRegionContext->MappedLength);
            }
            ACPI_FREE (LocalRegionContext);
            *RegionContext = NULL;
        }
        return_ACPI_STATUS (AE_OK);
    }

    /* Create a new context */

    LocalRegionContext = ACPI_ALLOCATE_ZEROED (sizeof (ACPI_MEM_SPACE_CONTEXT));
    if (!(LocalRegionContext))
    {
        return_ACPI_STATUS (AE_NO_MEMORY);
    }

    /* Save the region length and address for use in the handler */

    LocalRegionContext->Length  = RegionDesc->Region.Length;
    LocalRegionContext->Address = RegionDesc->Region.Address;

    *RegionContext = LocalRegionContext;
    return_ACPI_STATUS (AE_OK);
}


/*******************************************************************************
 *
 * FUNCTION:    AcpiEvIoSpaceRegionSetup
 *
 * PARAMETERS:  Handle              - Region we are interested in
 *              Function            - Start or stop
 *              HandlerContext      - Address space handler context
 *              RegionContext       - Region specific context
 *
 * RETURN:      Status
 *
 * DESCRIPTION: Setup a IO operation region
 *
 ******************************************************************************/

ACPI_STATUS
AcpiEvIoSpaceRegionSetup (
    ACPI_HANDLE             Handle,
    UINT32                  Function,
    void                    *HandlerContext,
    void                    **RegionContext)
{
    ACPI_FUNCTION_TRACE (EvIoSpaceRegionSetup);


    if (Function == ACPI_REGION_DEACTIVATE)
    {
        *RegionContext = NULL;
    }
    else
    {
        *RegionContext = HandlerContext;
    }

    return_ACPI_STATUS (AE_OK);
}


/*******************************************************************************
 *
 * FUNCTION:    AcpiEvPciConfigRegionSetup
 *
 * PARAMETERS:  Handle              - Region we are interested in
 *              Function            - Start or stop
 *              HandlerContext      - Address space handler context
 *              RegionContext       - Region specific context
 *
 * RETURN:      Status
 *
 * DESCRIPTION: Setup a PCI_Config operation region
 *
 * MUTEX:       Assumes namespace is not locked
 *
 ******************************************************************************/

ACPI_STATUS
AcpiEvPciConfigRegionSetup (
    ACPI_HANDLE             Handle,
    UINT32                  Function,
    void                    *HandlerContext,
    void                    **RegionContext)
{
    ACPI_STATUS             Status = AE_OK;
    UINT64                  PciValue;
    ACPI_PCI_ID             *PciId = *RegionContext;
    ACPI_OPERAND_OBJECT     *HandlerObj;
    ACPI_NAMESPACE_NODE     *ParentNode;
    ACPI_NAMESPACE_NODE     *PciRootNode;
    ACPI_NAMESPACE_NODE     *PciDeviceNode;
    ACPI_OPERAND_OBJECT     *RegionObj = (ACPI_OPERAND_OBJECT  *) Handle;


    ACPI_FUNCTION_TRACE (EvPciConfigRegionSetup);


    HandlerObj = RegionObj->Region.Handler;
    if (!HandlerObj)
    {
        /*
         * No installed handler. This shouldn't happen because the dispatch
         * routine checks before we get here, but we check again just in case.
         */
        ACPI_DEBUG_PRINT ((ACPI_DB_OPREGION,
            "Attempting to init a region %p, with no handler\n", RegionObj));
        return_ACPI_STATUS (AE_NOT_EXIST);
    }

    *RegionContext = NULL;
    if (Function == ACPI_REGION_DEACTIVATE)
    {
        if (PciId)
        {
            ACPI_FREE (PciId);
        }
        return_ACPI_STATUS (Status);
    }

    ParentNode = RegionObj->Region.Node->Parent;

    /*
     * Get the _SEG and _BBN values from the device upon which the handler
     * is installed.
     *
     * We need to get the _SEG and _BBN objects relative to the PCI BUS device.
     * This is the device the handler has been registered to handle.
     */

    /*
     * If the AddressSpace.Node is still pointing to the root, we need
     * to scan upward for a PCI Root bridge and re-associate the OpRegion
     * handlers with that device.
     */
    if (HandlerObj->AddressSpace.Node == AcpiGbl_RootNode)
    {
        /* Start search from the parent object */

        PciRootNode = ParentNode;
        while (PciRootNode != AcpiGbl_RootNode)
        {
            /* Get the _HID/_CID in order to detect a RootBridge */

            if (AcpiEvIsPciRootBridge (PciRootNode))
            {
                /* Install a handler for this PCI root bridge */

                Status = AcpiInstallAddressSpaceHandler (
                            (ACPI_HANDLE) PciRootNode,
                            ACPI_ADR_SPACE_PCI_CONFIG,
                            ACPI_DEFAULT_HANDLER, NULL, NULL);
                if (ACPI_FAILURE (Status))
                {
                    if (Status == AE_SAME_HANDLER)
                    {
                        /*
                         * It is OK if the handler is already installed on the
                         * root bridge. Still need to return a context object
                         * for the new PCI_Config operation region, however.
                         */
                        Status = AE_OK;
                    }
                    else
                    {
                        ACPI_EXCEPTION ((AE_INFO, Status,
                            "Could not install PciConfig handler "
                            "for Root Bridge %4.4s",
                            AcpiUtGetNodeName (PciRootNode)));
                    }
                }
                break;
            }

            PciRootNode = PciRootNode->Parent;
        }

        /* PCI root bridge not found, use namespace root node */
    }
    else
    {
        PciRootNode = HandlerObj->AddressSpace.Node;
    }

    /*
     * If this region is now initialized, we are done.
     * (InstallAddressSpaceHandler could have initialized it)
     */
    if (RegionObj->Region.Flags & AOPOBJ_SETUP_COMPLETE)
    {
        return_ACPI_STATUS (AE_OK);
    }

    /* Region is still not initialized. Create a new context */

    PciId = ACPI_ALLOCATE_ZEROED (sizeof (ACPI_PCI_ID));
    if (!PciId)
    {
        return_ACPI_STATUS (AE_NO_MEMORY);
    }

    /*
     * For PCI_Config space access, we need the segment, bus, device and
     * function numbers. Acquire them here.
     *
     * Find the parent device object. (This allows the operation region to be
     * within a subscope under the device, such as a control method.)
     */
    PciDeviceNode = RegionObj->Region.Node;
    while (PciDeviceNode && (PciDeviceNode->Type != ACPI_TYPE_DEVICE))
    {
        PciDeviceNode = PciDeviceNode->Parent;
    }

    if (!PciDeviceNode)
    {
        ACPI_FREE (PciId);
        return_ACPI_STATUS (AE_AML_OPERAND_TYPE);
    }

    /*
     * Get the PCI device and function numbers from the _ADR object
     * contained in the parent's scope.
     */
    Status = AcpiUtEvaluateNumericObject (METHOD_NAME__ADR,
                PciDeviceNode, &PciValue);

    /*
     * The default is zero, and since the allocation above zeroed the data,
     * just do nothing on failure.
     */
    if (ACPI_SUCCESS (Status))
    {
        PciId->Device   = ACPI_HIWORD (ACPI_LODWORD (PciValue));
        PciId->Function = ACPI_LOWORD (ACPI_LODWORD (PciValue));
    }

    /* The PCI segment number comes from the _SEG method */

    Status = AcpiUtEvaluateNumericObject (METHOD_NAME__SEG,
                PciRootNode, &PciValue);
    if (ACPI_SUCCESS (Status))
    {
        PciId->Segment = ACPI_LOWORD (PciValue);
    }

    /* The PCI bus number comes from the _BBN method */

    Status = AcpiUtEvaluateNumericObject (METHOD_NAME__BBN,
                PciRootNode, &PciValue);
    if (ACPI_SUCCESS (Status))
    {
        PciId->Bus = ACPI_LOWORD (PciValue);
    }

    /* Complete/update the PCI ID for this device */

    Status = AcpiHwDerivePciId (PciId, PciRootNode, RegionObj->Region.Node);
    if (ACPI_FAILURE (Status))
    {
        ACPI_FREE (PciId);
        return_ACPI_STATUS (Status);
    }

    *RegionContext = PciId;
    return_ACPI_STATUS (AE_OK);
}


/*******************************************************************************
 *
 * FUNCTION:    AcpiEvIsPciRootBridge
 *
 * PARAMETERS:  Node            - Device node being examined
 *
 * RETURN:      TRUE if device is a PCI/PCI-Express Root Bridge
 *
 * DESCRIPTION: Determine if the input device represents a PCI Root Bridge by
 *              examining the _HID and _CID for the device.
 *
 ******************************************************************************/

static BOOLEAN
AcpiEvIsPciRootBridge (
    ACPI_NAMESPACE_NODE     *Node)
{
    ACPI_STATUS             Status;
    ACPI_DEVICE_ID          *Hid;
    ACPI_DEVICE_ID_LIST     *Cid;
    UINT32                  i;
    BOOLEAN                 Match;


    /* Get the _HID and check for a PCI Root Bridge */

    Status = AcpiUtExecute_HID (Node, &Hid);
    if (ACPI_FAILURE (Status))
    {
        return (FALSE);
    }

    Match = AcpiUtIsPciRootBridge (Hid->String);
    ACPI_FREE (Hid);

    if (Match)
    {
        return (TRUE);
    }

    /* The _HID did not match. Get the _CID and check for a PCI Root Bridge */

    Status = AcpiUtExecute_CID (Node, &Cid);
    if (ACPI_FAILURE (Status))
    {
        return (FALSE);
    }

    /* Check all _CIDs in the returned list */

    for (i = 0; i < Cid->Count; i++)
    {
        if (AcpiUtIsPciRootBridge (Cid->Ids[i].String))
        {
            ACPI_FREE (Cid);
            return (TRUE);
        }
    }

    ACPI_FREE (Cid);
    return (FALSE);
}


/*******************************************************************************
 *
 * FUNCTION:    AcpiEvPciBarRegionSetup
 *
 * PARAMETERS:  Handle              - Region we are interested in
 *              Function            - Start or stop
 *              HandlerContext      - Address space handler context
 *              RegionContext       - Region specific context
 *
 * RETURN:      Status
 *
 * DESCRIPTION: Setup a PciBAR operation region
 *
 * MUTEX:       Assumes namespace is not locked
 *
 ******************************************************************************/

ACPI_STATUS
AcpiEvPciBarRegionSetup (
    ACPI_HANDLE             Handle,
    UINT32                  Function,
    void                    *HandlerContext,
    void                    **RegionContext)
{
    ACPI_FUNCTION_TRACE (EvPciBarRegionSetup);


    return_ACPI_STATUS (AE_OK);
}


/*******************************************************************************
 *
 * FUNCTION:    AcpiEvCmosRegionSetup
 *
 * PARAMETERS:  Handle              - Region we are interested in
 *              Function            - Start or stop
 *              HandlerContext      - Address space handler context
 *              RegionContext       - Region specific context
 *
 * RETURN:      Status
 *
 * DESCRIPTION: Setup a CMOS operation region
 *
 * MUTEX:       Assumes namespace is not locked
 *
 ******************************************************************************/

ACPI_STATUS
AcpiEvCmosRegionSetup (
    ACPI_HANDLE             Handle,
    UINT32                  Function,
    void                    *HandlerContext,
    void                    **RegionContext)
{
    ACPI_FUNCTION_TRACE (EvCmosRegionSetup);


    return_ACPI_STATUS (AE_OK);
}


/*******************************************************************************
 *
 * FUNCTION:    AcpiEvDefaultRegionSetup
 *
 * PARAMETERS:  Handle              - Region we are interested in
 *              Function            - Start or stop
 *              HandlerContext      - Address space handler context
 *              RegionContext       - Region specific context
 *
 * RETURN:      Status
 *
 * DESCRIPTION: Default region initialization
 *
 ******************************************************************************/

ACPI_STATUS
AcpiEvDefaultRegionSetup (
    ACPI_HANDLE             Handle,
    UINT32                  Function,
    void                    *HandlerContext,
    void                    **RegionContext)
{
    ACPI_FUNCTION_TRACE (EvDefaultRegionSetup);


    if (Function == ACPI_REGION_DEACTIVATE)
    {
        *RegionContext = NULL;
    }
    else
    {
        *RegionContext = HandlerContext;
    }

    return_ACPI_STATUS (AE_OK);
}


/*******************************************************************************
 *
 * FUNCTION:    AcpiEvInitializeRegion
 *
 * PARAMETERS:  RegionObj       - Region we are initializing
 *              AcpiNsLocked    - Is namespace locked?
 *
 * RETURN:      Status
 *
 * DESCRIPTION: Initializes the region, finds any _REG methods and saves them
 *              for execution at a later time
 *
 *              Get the appropriate address space handler for a newly
 *              created region.
 *
 *              This also performs address space specific initialization. For
 *              example, PCI regions must have an _ADR object that contains
 *              a PCI address in the scope of the definition. This address is
 *              required to perform an access to PCI config space.
 *
 * MUTEX:       Interpreter should be unlocked, because we may run the _REG
 *              method for this region.
 *
 ******************************************************************************/

ACPI_STATUS
AcpiEvInitializeRegion (
    ACPI_OPERAND_OBJECT     *RegionObj,
    BOOLEAN                 AcpiNsLocked)
{
    ACPI_OPERAND_OBJECT     *HandlerObj;
    ACPI_OPERAND_OBJECT     *ObjDesc;
    ACPI_ADR_SPACE_TYPE     SpaceId;
    ACPI_NAMESPACE_NODE     *Node;
    ACPI_STATUS             Status;
    ACPI_NAMESPACE_NODE     *MethodNode;
    ACPI_NAME               *RegNamePtr = (ACPI_NAME *) METHOD_NAME__REG;
    ACPI_OPERAND_OBJECT     *RegionObj2;


    ACPI_FUNCTION_TRACE_U32 (EvInitializeRegion, AcpiNsLocked);


    if (!RegionObj)
    {
        return_ACPI_STATUS (AE_BAD_PARAMETER);
    }

    if (RegionObj->Common.Flags & AOPOBJ_OBJECT_INITIALIZED)
    {
        return_ACPI_STATUS (AE_OK);
    }

    RegionObj2 = AcpiNsGetSecondaryObject (RegionObj);
    if (!RegionObj2)
    {
        return_ACPI_STATUS (AE_NOT_EXIST);
    }

    Node = RegionObj->Region.Node->Parent;
    SpaceId = RegionObj->Region.SpaceId;

    /* Setup defaults */

    RegionObj->Region.Handler = NULL;
    RegionObj2->Extra.Method_REG = NULL;
    RegionObj->Common.Flags &= ~(AOPOBJ_SETUP_COMPLETE);
    RegionObj->Common.Flags |= AOPOBJ_OBJECT_INITIALIZED;

    /* Find any "_REG" method associated with this region definition */

    Status = AcpiNsSearchOneScope (
                *RegNamePtr, Node, ACPI_TYPE_METHOD, &MethodNode);
    if (ACPI_SUCCESS (Status))
    {
        /*
         * The _REG method is optional and there can be only one per region
         * definition. This will be executed when the handler is attached
         * or removed
         */
        RegionObj2->Extra.Method_REG = MethodNode;
    }

    /*
     * The following loop depends upon the root Node having no parent
     * ie: AcpiGbl_RootNode->ParentEntry being set to NULL
     */
    while (Node)
    {
        /* Check to see if a handler exists */

        HandlerObj = NULL;
        ObjDesc = AcpiNsGetAttachedObject (Node);
        if (ObjDesc)
        {
            /* Can only be a handler if the object exists */

            switch (Node->Type)
            {
            case ACPI_TYPE_DEVICE:

                HandlerObj = ObjDesc->Device.Handler;
                break;

            case ACPI_TYPE_PROCESSOR:

                HandlerObj = ObjDesc->Processor.Handler;
                break;

            case ACPI_TYPE_THERMAL:

                HandlerObj = ObjDesc->ThermalZone.Handler;
                break;

            case ACPI_TYPE_METHOD:
                /*
                 * If we are executing module level code, the original
                 * Node's object was replaced by this Method object and we
                 * saved the handler in the method object.
                 *
                 * See AcpiNsExecModuleCode
                 */
                if (ObjDesc->Method.InfoFlags & ACPI_METHOD_MODULE_LEVEL)
                {
                    HandlerObj = ObjDesc->Method.Dispatch.Handler;
                }
                break;

            default:
                /* Ignore other objects */
                break;
            }

            while (HandlerObj)
            {
                /* Is this handler of the correct type? */

                if (HandlerObj->AddressSpace.SpaceId == SpaceId)
                {
                    /* Found correct handler */

                    ACPI_DEBUG_PRINT ((ACPI_DB_OPREGION,
                        "Found handler %p for region %p in obj %p\n",
                        HandlerObj, RegionObj, ObjDesc));

                    Status = AcpiEvAttachRegion (HandlerObj, RegionObj,
                                AcpiNsLocked);

                    /*
                     * Tell all users that this region is usable by
                     * running the _REG method
                     */
                    if (AcpiNsLocked)
                    {
                        Status = AcpiUtReleaseMutex (ACPI_MTX_NAMESPACE);
                        if (ACPI_FAILURE (Status))
                        {
                            return_ACPI_STATUS (Status);
                        }
                    }

                    Status = AcpiEvExecuteRegMethod (RegionObj, ACPI_REG_CONNECT);

                    if (AcpiNsLocked)
                    {
                        Status = AcpiUtAcquireMutex (ACPI_MTX_NAMESPACE);
                        if (ACPI_FAILURE (Status))
                        {
                            return_ACPI_STATUS (Status);
                        }
                    }

                    return_ACPI_STATUS (AE_OK);
                }

                /* Try next handler in the list */

                HandlerObj = HandlerObj->AddressSpace.Next;
            }
        }

        /* This node does not have the handler we need; Pop up one level */

        Node = Node->Parent;
    }

    /* If we get here, there is no handler for this region */

    ACPI_DEBUG_PRINT ((ACPI_DB_OPREGION,
        "No handler for RegionType %s(%X) (RegionObj %p)\n",
        AcpiUtGetRegionName (SpaceId), SpaceId, RegionObj));

    return_ACPI_STATUS (AE_NOT_EXIST);
}

