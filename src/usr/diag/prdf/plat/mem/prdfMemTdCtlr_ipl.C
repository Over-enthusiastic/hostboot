/* IBM_PROLOG_BEGIN_TAG                                                   */
/* This is an automatically generated prolog.                             */
/*                                                                        */
/* $Source: src/usr/diag/prdf/plat/mem/prdfMemTdCtlr_ipl.C $              */
/*                                                                        */
/* OpenPOWER HostBoot Project                                             */
/*                                                                        */
/* Contributors Listed Below - COPYRIGHT 2016,2017                        */
/* [+] International Business Machines Corp.                              */
/*                                                                        */
/*                                                                        */
/* Licensed under the Apache License, Version 2.0 (the "License");        */
/* you may not use this file except in compliance with the License.       */
/* You may obtain a copy of the License at                                */
/*                                                                        */
/*     http://www.apache.org/licenses/LICENSE-2.0                         */
/*                                                                        */
/* Unless required by applicable law or agreed to in writing, software    */
/* distributed under the License is distributed on an "AS IS" BASIS,      */
/* WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or        */
/* implied. See the License for the specific language governing           */
/* permissions and limitations under the License.                         */
/*                                                                        */
/* IBM_PROLOG_END_TAG                                                     */

/** @file  prdfMemTdCtlr_ipl.C
 *  @brief A state machine for memory Targeted Diagnostics (IPL only).
 */

#include <prdfMemTdCtlr.H>

// Platform includes
#include <prdfMemEccAnalysis.H>
#include <prdfMemMark.H>
#include <prdfMemoryMru.H>
#include <prdfMemScrubUtils.H>
#include <prdfMemUtils.H>
#include <prdfMemVcm.H>
#include <prdfP9McaDataBundle.H>
#include <prdfP9McaExtraSig.H>

using namespace TARGETING;

namespace PRDF
{

using namespace PlatServices;

//------------------------------------------------------------------------------

template <TARGETING::TYPE T>
uint32_t MemTdCtlr<T>::handleTdEvent( STEP_CODE_DATA_STRUCT & io_sc,
                                      TdEntry * i_entry )
{
    #define PRDF_FUNC "[MemTdCtlr::handleTdEvent] "

    // Add this entry to the queue.
    iv_queue.push( i_entry );

    return SUCCESS;

    #undef PRDF_FUNC
}

//------------------------------------------------------------------------------

template <TARGETING::TYPE T>
uint32_t MemTdCtlr<T>::defaultStep( STEP_CODE_DATA_STRUCT & io_sc )
{
    #define PRDF_FUNC "[MemTdCtlr::defaultStep] "

    uint32_t o_rc = SUCCESS;

    TdRankListEntry nextRank = iv_rankList.getNext( iv_stoppedRank,
                                                    iv_broadcastMode );

    do
    {
        if ( nextRank <= iv_stoppedRank ) // The command made it to the end.
        {
            PRDF_TRAC( PRDF_FUNC "The TD command made it to the end of "
                       "memory on chip: 0x%08x", iv_chip->getHuid() );

            // Clear all of the counters and maintenance ECC attentions. This
            // must be done before telling MDIA the command is done. Otherwise,
            // we may run into a race condition where MDIA may start the next
            // command and it completes before PRD clears the FIR bits for this
            // attention.
            o_rc = prepareNextCmd<T>( iv_chip );
            if ( SUCCESS != o_rc )
            {
                PRDF_ERR( PRDF_FUNC "prepareNextCmd<T>(0x%08x) failed",
                          iv_chip->getHuid() );
                break;
            }

            // The command reached the end of memory. Send a message to MDIA.
            o_rc = mdiaSendEventMsg(iv_chip->getTrgt(), MDIA::COMMAND_COMPLETE);
            if ( SUCCESS != o_rc )
            {
                PRDF_ERR( PRDF_FUNC "mdiaSendEventMsg(0x%08x,COMMAND_COMPLETE) "
                          "failed", iv_chip->getHuid() );
                break;
            }
        }
        else // There is memory left to test.
        {
            PRDF_TRAC( PRDF_FUNC "There is still memory left to test. "
                       "Calling startSfRead<T>(0x%08x, m%ds%d)",
                       nextRank.getChip()->getHuid(),
                       nextRank.getRank().getMaster(),
                       nextRank.getRank().getSlave() );

            // Start a super fast command to the end of memory.
            o_rc = startSfRead<T>( nextRank.getChip(), nextRank.getRank() );
            if ( SUCCESS != o_rc )
            {
                PRDF_ERR( PRDF_FUNC "startSfRead<T>(0x%08x,m%ds%d) failed",
                          nextRank.getChip()->getHuid(),
                          nextRank.getRank().getMaster(),
                          nextRank.getRank().getSlave() );
                break;
            }
        }

    } while (0);

    return o_rc;

    #undef PRDF_FUNC
}

//------------------------------------------------------------------------------

template <TARGETING::TYPE T, typename D>
uint32_t __checkEcc( ExtensibleChip * i_chip, TdQueue & io_queue,
                     const MemAddr & i_addr, bool & o_errorsFound,
                     STEP_CODE_DATA_STRUCT & io_sc )
{
    #define PRDF_FUNC "[__checkEcc] "

    uint32_t o_rc = SUCCESS;

    o_errorsFound = true; // Assume true for unless nothing found.

    TargetHandle_t trgt = i_chip->getTrgt();
    HUID           huid = i_chip->getHuid();

    MemRank rank = i_addr.getRank();

    do
    {
        // Check for ECC errors.
        uint32_t eccAttns = 0;
        o_rc = checkEccFirs<T>( i_chip, eccAttns );
        if ( SUCCESS != o_rc )
        {
            PRDF_ERR( PRDF_FUNC "checkEccFirs<T>(0x%08x) failed", huid );
            break;
        }

        if ( 0 != (eccAttns & MAINT_UE) )
        {
            // Add the signature to the multi-signature list. Also, since
            // this will be a predictive callout, change the primary
            // signature as well.
            io_sc.service_data->AddSignatureList( trgt, PRDFSIG_MaintUE );
            io_sc.service_data->setSignature(     huid, PRDFSIG_MaintUE );

            // Do memory UE handling.
            o_rc = MemEcc::handleMemUe<T>( i_chip, i_addr, UE_TABLE::SCRUB_UE,
                                           io_sc );
            if ( SUCCESS != o_rc )
            {
                PRDF_ERR( PRDF_FUNC "handleMemUe<T>(0x%08x) failed",
                          i_chip->getHuid() );
                break;
            }
        }
        else if ( 0 != (eccAttns & MAINT_MPE) )
        {
            io_sc.service_data->AddSignatureList( trgt, PRDFSIG_MaintMPE );

            o_rc = MemEcc::handleMpe<T,D>( i_chip, rank, io_sc );
            if ( SUCCESS != o_rc )
            {
                PRDF_ERR( PRDF_FUNC "handleMpe<T>(0x%08x, 0x%02x) failed",
                          i_chip->getHuid(), rank.getKey() );
                break;
            }
        }
        else if ( isMfgCeCheckingEnabled() &&
                  (0 != (eccAttns & MAINT_HARD_NCE_ETE)) &&
                  ( (0 != (eccAttns & MAINT_NCE)) ||
                    (0 != (eccAttns & MAINT_TCE))    ) )
        {
            // NOTE: The MAINT_HARD_NCE_ETE attention is reported on the MCBIST.
            //       If the command is run in broadcast mode, we may end up
            //       doing TPS on all four ports when only one port has the CE.
            //       Therefore, we must check for MAINT_NCE or MAINT_TCE, which
            //       are found on the MCA, to determine if this MCA need a TPS
            //       procedure.

            io_sc.service_data->AddSignatureList( trgt, PRDFSIG_MaintHARD_CTE );

            // NOTE: Normally we would add each symbol in the per-symbol
            //       counters to the callout list for FFDC, but the results are
            //       a bit undefined in broadcast mode. Since this should not be
            //       a predictive error log, it is fine to have no callouts for
            //       now.

            // Add a TPS procedure to the queue.
            TdEntry * e = new TpsEvent<T>{ i_chip, rank };
            io_queue.push( e );
        }
        else // Nothing found.
        {
            o_errorsFound = false;
        }

    } while (0);

    return o_rc;

    #undef PRDF_FUNC
}

template
uint32_t __checkEcc<TYPE_MCA, McaDataBundle *>( ExtensibleChip * i_chip,
                                                TdQueue & io_queue,
                                                const MemAddr & i_addr,
                                                bool & o_errorsFound,
                                                STEP_CODE_DATA_STRUCT & io_sc );

/* TODO RTC 157888
template
uint32_t __checkEcc<TYPE_MBA>( ExtensibleChip * i_chip, TdQueue & io_queue,
                               const MemAddr & i_addr, bool & o_errorsFound,
                               STEP_CODE_DATA_STRUCT & io_sc );
*/

//------------------------------------------------------------------------------

// Avoid linker errors with the template.
template class MemTdCtlr<TYPE_MCBIST>;
template class MemTdCtlr<TYPE_MBA>;

//------------------------------------------------------------------------------

} // end namespace PRDF

