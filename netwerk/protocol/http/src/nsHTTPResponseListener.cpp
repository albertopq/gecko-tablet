/* -*- Mode: C++; tab-width: 4; indent-tabs-mode: nil; c-basic-offset: 4 -*-
 *
 * The contents of this file are subject to the Netscape Public License
 * Version 1.0 (the "NPL"); you may not use this file except in
 * compliance with the NPL.  You may obtain a copy of the NPL at
 * http://www.mozilla.org/NPL/
 *
 * Software distributed under the NPL is distributed on an "AS IS" basis,
 * WITHOUT WARRANTY OF ANY KIND, either express or implied. See the NPL
 * for the specific language governing rights and limitations under the
 * NPL.
 *
 * The Initial Developer of this code under the NPL is Netscape
 * Communications Corporation.  Portions created by Netscape are
 * Copyright (C) 1998 Netscape Communications Corporation.  All Rights
 * Reserved.
 */

#include "nsIStreamListener.h"
#include "nsHTTPResponseListener.h"
#include "nsIChannel.h"
#include "nsIBufferInputStream.h"
#include "nsHTTPChannel.h"
#include "nsHTTPResponse.h"
#include "nsIHttpEventSink.h"
#include "nsCRT.h"

#include "nsIHttpNotify.h"
#include "nsINetModRegEntry.h"
#include "nsProxyObjectManager.h"
#include "nsIServiceManager.h"
#include "nsINetModuleMgr.h"
#include "nsIEventQueueService.h"
#include "nsIBuffer.h"

//
// This specifies the maximum allowable size for a server Status-Line
// or Response-Header.
//
static const int kMAX_HEADER_SIZE = 60000;


nsHTTPResponseListener::nsHTTPResponseListener(): 
    m_pConnection(nsnull),
    m_bFirstLineParsed(PR_FALSE),
    m_pResponse(nsnull),
    m_pConsumer(nsnull),
    m_ReadLength(0),
    m_bHeadersDone(PR_FALSE),
    m_HeaderBuffer(eOneByte)
{
    NS_INIT_REFCNT();
}

nsHTTPResponseListener::~nsHTTPResponseListener()
{
    NS_IF_RELEASE(m_pConnection);
    NS_IF_RELEASE(m_pResponse);
    NS_IF_RELEASE(m_pConsumer);
}

NS_IMPL_ISUPPORTS(nsHTTPResponseListener,nsIStreamListener::GetIID());

static NS_DEFINE_IID(kProxyObjectManagerIID, NS_IPROXYEVENT_MANAGER_IID);
static NS_DEFINE_CID(kEventQueueService, NS_EVENTQUEUESERVICE_CID);
static NS_DEFINE_CID(kNetModuleMgrCID, NS_NETMODULEMGR_CID);

NS_IMETHODIMP
nsHTTPResponseListener::OnDataAvailable(nsISupports* context,
                                        nsIBufferInputStream *i_pStream, 
                                        PRUint32 i_SourceOffset,
                                        PRUint32 i_Length)
{
    nsresult rv = NS_OK;
    PRUint32 actualBytesRead;
    NS_ASSERTION(i_pStream, "No stream supplied by the transport!");

    if (!m_pResponse)
    {
        // why do I need the connection in the constructor... get rid.. TODO
        m_pResponse = new nsHTTPResponse (i_pStream);
        if (!m_pResponse)
        {
            NS_ERROR("Failed to create the response object!");
            return NS_ERROR_OUT_OF_MEMORY;
        }
        NS_ADDREF(m_pResponse);
        nsHTTPChannel* pTestCon = NS_STATIC_CAST(nsHTTPChannel*, m_pConnection);
        pTestCon->SetResponse(m_pResponse);
    }

    if (!m_bHeadersDone) {
      nsCOMPtr<nsIBuffer> pBuffer;

      rv = i_pStream->GetBuffer(getter_AddRefs(pBuffer));
      if (NS_FAILED(rv)) return rv;

      if (!m_bFirstLineParsed) {
        rv = ParseStatusLine(pBuffer, i_Length, &actualBytesRead);
        i_Length -= actualBytesRead;
      }

      while (NS_SUCCEEDED(rv) && i_Length && !m_bHeadersDone) {
        rv = ParseHTTPHeader(pBuffer, i_Length, &actualBytesRead);
        i_Length -= actualBytesRead;
      }

      if (NS_FAILED(rv)) return rv;
    }

    NS_ASSERTION(m_pConsumer, "No Stream Listener!");
    if (i_Length && m_pConsumer) {
      rv = m_pConsumer->OnDataAvailable(m_pConnection, i_pStream, 0, i_Length);
    }

    return rv;
}


NS_IMETHODIMP
nsHTTPResponseListener::OnStartBinding(nsISupports* i_pContext)
{
    nsresult rv;

    //TODO globally replace printf with trace calls. 
    //printf("nsHTTPResponseListener::OnStartBinding...\n");

    // Initialize header varaibles...  
    m_bHeadersDone     = PR_FALSE;
    m_bFirstLineParsed = PR_FALSE;

    // Cache the nsIHTTPChannel...
    if (i_pContext) {
        rv = i_pContext->QueryInterface(nsIHTTPChannel::GetIID(), 
                                        (void**)&m_pConnection);
    } else {
        rv = NS_ERROR_NULL_POINTER;
    }

    // Cache the nsIStreamListener of the consumer...
    if (NS_SUCCEEDED(rv)) {
        rv = m_pConnection->GetResponseDataListener(&m_pConsumer);
    }

    NS_ASSERTION(m_pConsumer, "No Stream Listener!");

    // Pass the notification out to the consumer...
    if (m_pConsumer) {
        // XXX: This is the wrong context being passed out to the consumer
        rv = m_pConsumer->OnStartBinding(i_pContext);
    }

    return rv;
}

NS_IMETHODIMP
nsHTTPResponseListener::OnStopBinding(nsISupports* i_pContext,
                                 nsresult i_Status,
                                 const PRUnichar* i_pMsg)
{
    nsresult rv;
    //printf("nsHTTPResponseListener::OnStopBinding...\n");
    //NS_ASSERTION(m_pResponse, "Response object not created yet or died?!");

    NS_ASSERTION(m_pConsumer, "No Stream Listener!");
    // Pass the notification out to the consumer...
    if (m_pConsumer) {
        // XXX: This is the wrong context being passed out to the consumer
        rv = m_pConsumer->OnStopBinding(i_pContext, i_Status, i_pMsg);
    } else {
      rv = NS_ERROR_FAILURE;
    }

    // The Consumer is no longer needed...
    NS_IF_RELEASE(m_pConsumer);

    // The HTTPChannel is no longer needed...
    NS_IF_RELEASE(m_pConnection);

    return rv;
}

NS_IMETHODIMP
nsHTTPResponseListener::OnStartRequest(nsISupports* i_pContext)
{
    return NS_ERROR_NOT_IMPLEMENTED;
}

NS_IMETHODIMP
nsHTTPResponseListener::OnStopRequest(nsISupports* i_pContext,
                                      nsresult iStatus,
                                      const PRUnichar* i_pMsg)
{
    return NS_ERROR_NOT_IMPLEMENTED;
}


nsresult nsHTTPResponseListener::FireOnHeadersAvailable()
{
    nsresult rv;
    NS_ASSERTION(m_bHeadersDone, "Headers have not been received!");

    if (m_bHeadersDone) {

        // Notify the event sink that response headers are available...
        nsIHTTPEventSink* pSink= nsnull;
        m_pConnection->GetEventSink(&pSink);
        if (pSink) {
            pSink->OnHeadersAvailable(m_pConnection);
        }

        // Check for any modules that want to receive headers once they've arrived.
        NS_WITH_SERVICE(nsINetModuleMgr, pNetModuleMgr, kNetModuleMgrCID, &rv);
        if (NS_FAILED(rv)) return rv;

        nsISimpleEnumerator* pModules = nsnull;
        rv = pNetModuleMgr->EnumerateModules("http-response", &pModules);
        if (NS_FAILED(rv)) return rv;

        nsIProxyObjectManager*  proxyObjectManager = nsnull; 
        rv = nsServiceManager::GetService( NS_XPCOMPROXY_PROGID, 
                                            kProxyObjectManagerIID,
                                            (nsISupports **)&proxyObjectManager);
        if (NS_FAILED(rv)) {
            NS_RELEASE(pModules);
            return rv;
        }

        nsISupports *supEntry = nsnull;

        // Go through the external modules and notify each one.
        rv = pModules->GetNext(&supEntry);
        while (NS_SUCCEEDED(rv)) {
            nsINetModRegEntry *entry = nsnull;
            rv = supEntry->QueryInterface(nsINetModRegEntry::GetIID(), (void**)&entry);
            NS_RELEASE(supEntry);
            if (NS_FAILED(rv)) {
                NS_RELEASE(pModules);
                NS_RELEASE(proxyObjectManager);
                return rv;
            }

            nsCID *lCID;
            nsIEventQueue* lEventQ = nsnull;

            rv = entry->GetMCID(&lCID);
            if (NS_FAILED(rv)) {
                NS_RELEASE(pModules);
                NS_RELEASE(proxyObjectManager);
                return rv;
            }

            rv = entry->GetMEventQ(&lEventQ);
            if (NS_FAILED(rv)) {
                NS_RELEASE(pModules);
                NS_RELEASE(proxyObjectManager);            
                return rv;
            }

            nsIHTTPNotify *pNotify = nsnull;
            // if this call fails one of the following happened.
            // a) someone registered an object for this topic but didn't
            //    implement the nsIHTTPNotify interface on that object.
            // b) someone registered an object for this topic bud didn't
            //    put the .xpt lib for that object in the components dir
            rv = proxyObjectManager->GetProxyObject(lEventQ, 
                                               *lCID,
                                               nsnull,
                                               nsIHTTPNotify::GetIID(),
                                               PROXY_ASYNC,
                                               (void**)&pNotify);
            NS_RELEASE(proxyObjectManager);
        
            NS_RELEASE(lEventQ);

            if (NS_SUCCEEDED(rv)) {
                // send off the notification, and block.
                nsIHTTPNotify* externMod = nsnull;
                rv = pNotify->QueryInterface(nsIHTTPNotify::GetIID(), (void**)&externMod);
                NS_RELEASE(pNotify);
            
                if (NS_FAILED(rv)) {
                    NS_ASSERTION(0, "proxy object manager found an interface we can not QI for");
                    NS_RELEASE(pModules);
                    return rv;
                }

                // make the nsIHTTPNotify api call
                externMod->AsyncExamineResponse(m_pConnection);
                NS_RELEASE(externMod);
                // we could do something with the return code from the external
                // module, but what????            
            }

            NS_RELEASE(entry);
            rv = pModules->GetNext(&supEntry); // go around again
        }
        NS_RELEASE(pModules);
        NS_IF_RELEASE(proxyObjectManager);

    } else {
        rv = NS_ERROR_FAILURE;
    }

    return rv;
}

NS_METHOD
nsWriteToString(void* closure,
                const char* fromRawSegment,
                PRUint32 offset,
                PRUint32 count,
                PRUint32 *writeCount)
{
  nsString *str = (nsString*)closure;

  str->Append(fromRawSegment, count);
  *writeCount = count;
  
  return NS_OK;
}


nsresult nsHTTPResponseListener::ParseStatusLine(nsIBuffer* aBuffer, 
                                                 PRUint32 aLength,
                                                 PRUint32 *aBytesRead)
{
  nsresult rv = NS_OK;

  PRBool bFoundString = PR_FALSE;
  PRUint32 offsetOfEnd, totalBytesToRead, actualBytesRead;

  *aBytesRead = 0;

  if (kMAX_HEADER_SIZE < m_HeaderBuffer.Length()) {
    // This server is yanking our chain...
    return NS_ERROR_FAILURE;
  }

  // Look for the LF which ends the Status-Line.
  rv = aBuffer->Search("\n", PR_FALSE, &bFoundString, &offsetOfEnd);
  if (NS_FAILED(rv)) return rv;

  if (!bFoundString) {
    //
    // This is a partial header...  Read the entire buffer and wait for
    // more data...
    //
    totalBytesToRead = aLength;
  } else {
    // Do not forget to include the LF character in the read...
    totalBytesToRead = offsetOfEnd+1;
  }

  rv = aBuffer->ReadSegments(nsWriteToString, 
                             (void*)&m_HeaderBuffer, 
                             totalBytesToRead, 
                             &actualBytesRead);
  if (NS_FAILED(rv)) return rv;

  *aBytesRead += actualBytesRead;

  // Wait for more data to arrive before processing the header...
  if (!bFoundString) return NS_OK;

  //
  // Replace all LWS with single SP characters.  Also remove the CRLF
  // characters...
  //
  m_HeaderBuffer.CompressSet(" \t", ' ');
  m_HeaderBuffer.StripChars("\r\n");

  //
  // The Status Line has the following: format:
  //    HTTP-Version SP Status-Code SP Reason-Phrase CRLF
  //

  const char *token;
  nsAutoString str(eOneByte);
  PRInt32 offset, error;

  //
  // Parse the HTTP-Version:: "HTTP" "/" 1*DIGIT "." 1*DIGIT
  //

  offset = m_HeaderBuffer.Find(' ');
  (void) m_HeaderBuffer.Left(str, offset);
  if (!str.Length()) {
    // The status line is bogus...
    return NS_ERROR_FAILURE;
  }
  token = str.GetBuffer();
  m_pResponse->SetServerVersion(token);

  m_HeaderBuffer.Cut(0, offset+1);

  //
  // Parse the Status-Code:: 3DIGIT
  //
  PRInt32 statusCode;

  offset = m_HeaderBuffer.Find(' ');
  (void) m_HeaderBuffer.Left(str, offset);
  if (3 != str.Length()) {
    // The status line is bogus...
    return NS_ERROR_FAILURE;
  }

  statusCode = str.ToInteger(&error);
  if (NS_FAILED(error)) return NS_ERROR_FAILURE;

  m_pResponse->SetStatus(statusCode);
  
  m_HeaderBuffer.Cut(0, offset+1);

  //
  // Parse the Reason-Phrase:: *<TEXT excluding CR,LF>
  //
  if (!m_HeaderBuffer.Length()) {
    // The status line is bogus...
    return NS_ERROR_FAILURE;
  }
  token = m_HeaderBuffer.GetBuffer();
  m_pResponse->SetStatusString(token);

  m_HeaderBuffer.Truncate();
  m_bFirstLineParsed = PR_TRUE;
  
  return rv;
}



nsresult nsHTTPResponseListener::ParseHTTPHeader(nsIBuffer* aBuffer,
                                                 PRUint32 aLength,
                                                 PRUint32 *aBytesRead)
{
  nsresult rv = NS_OK;

  const char *buf;
  PRBool bFoundString;
  PRUint32 offsetOfEnd, totalBytesToRead, actualBytesRead;

  *aBytesRead = 0;

  if (kMAX_HEADER_SIZE < m_HeaderBuffer.Length()) {
    // This server is yanking our chain...
    return NS_ERROR_FAILURE;
  }

  //
  // Read the header from the input buffer...  A header is terminated by 
  // a CRLF.  Header values may be extended over multiple lines by preceeding
  // each extran line with LWS...
  //
  do {
    //
    // If last character in the header string is a LF, then the header 
    // may be complete...
    //
    if (m_HeaderBuffer.Last() == '\n' ) {
      rv = aBuffer->GetReadSegment(0, &buf, &actualBytesRead);
      // Need to wait for more data to see if the header is complete.
      if (0 == actualBytesRead) {
        return NS_OK;
      }

      // Not LWS - The header is complete...
      if ((*buf != ' ') && (*buf != '\t')) {
        break;
      }
    }

    // Look for the next LF in the buffer...
    rv = aBuffer->Search("\n", PR_FALSE, &bFoundString, &offsetOfEnd);
    if (NS_FAILED(rv)) return rv;

    if (!bFoundString) {
      //
      // The buffer contains a partial header.  Read the entire buffer 
      // and wait for more data...
      //
      totalBytesToRead = aLength;
    } else {
    // Do not forget to include the LF character in the read...
      totalBytesToRead = offsetOfEnd+1;
    }

    // Append the buffer into the header string...
    rv = aBuffer->ReadSegments(nsWriteToString, 
                               (void*)&m_HeaderBuffer, 
                               totalBytesToRead, 
                               &actualBytesRead);
    if (NS_FAILED(rv)) return rv;

    *aBytesRead += actualBytesRead;

    // Partial header - wait for more data to arrive...
    if (!bFoundString) return NS_OK;

  } while (PR_TRUE);

  //
  // Replace all LWS with single SP characters.  And remove all of the CRLF
  // characters...
  //
  m_HeaderBuffer.CompressSet(" \t", ' ');
  m_HeaderBuffer.StripChars("\r\n");

  if (!m_HeaderBuffer.Length()) {
    m_bHeadersDone = PR_TRUE;
    return NS_OK;
  }

  nsAutoString headerKey(eOneByte);
  PRInt32 colonOffset;

  // Extract the key field - everything up to the ':'
  // The header name is case-insensitive...
  colonOffset = m_HeaderBuffer.Find(':');
  (void) m_HeaderBuffer.Left(headerKey, colonOffset);
  headerKey.ToLowerCase();

  // Extract the value field - everything past the ':'
  // Trim any leading or trailing whitespace...
  m_HeaderBuffer.Cut(0, colonOffset+1);
  m_HeaderBuffer.Trim(" ");

  rv = m_pResponse->SetHeaderInternal(headerKey.GetBuffer(), m_HeaderBuffer.GetBuffer());

  m_HeaderBuffer.Truncate();

  return rv;
}