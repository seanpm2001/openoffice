/**************************************************************
 * 
 * Licensed to the Apache Software Foundation (ASF) under one
 * or more contributor license agreements.  See the NOTICE file
 * distributed with this work for additional information
 * regarding copyright ownership.  The ASF licenses this file
 * to you under the Apache License, Version 2.0 (the
 * "License"); you may not use this file except in compliance
 * with the License.  You may obtain a copy of the License at
 * 
 *   http://www.apache.org/licenses/LICENSE-2.0
 * 
 * Unless required by applicable law or agreed to in writing,
 * software distributed under the License is distributed on an
 * "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY
 * KIND, either express or implied.  See the License for the
 * specific language governing permissions and limitations
 * under the License.
 * 
 *************************************************************/



// MARKER(update_precomp.py): autogen include statement, do not remove
#include "precompiled_scripting.hxx"
#include "scripthandler.hxx"

#include <osl/mutex.hxx>

#include <com/sun/star/frame/DispatchResultEvent.hpp>
#include <com/sun/star/frame/DispatchResultState.hpp>
#include <com/sun/star/frame/XController.hpp>
#include <com/sun/star/frame/XModel.hpp>

#include <com/sun/star/document/XEmbeddedScripts.hpp>
#include <com/sun/star/document/XScriptInvocationContext.hpp>

#include <com/sun/star/lang/XSingleServiceFactory.hpp>

#include <com/sun/star/script/provider/XScriptProviderSupplier.hpp>
#include <com/sun/star/script/provider/XScriptProviderFactory.hpp>
#include <com/sun/star/script/provider/ScriptFrameworkErrorType.hpp>

#include <sfx2/objsh.hxx>
#include <sfx2/frame.hxx>
#include <sfx2/sfxdlg.hxx>
#include <vcl/abstdlg.hxx>
#include <tools/diagnose_ex.h>

#include <cppuhelper/factory.hxx>
#include <cppuhelper/exc_hlp.hxx>
#include <cppuhelper/implementationentry.hxx>
#include <util/util.hxx>
#include <framework/documentundoguard.hxx>

#include "com/sun/star/uno/XComponentContext.hpp"
#include "com/sun/star/uri/XUriReference.hpp"
#include "com/sun/star/uri/UriReferenceFactory.hpp"
#include "com/sun/star/uri/XUriReferenceFactory.hpp"
#include "com/sun/star/uri/XVndSunStarScriptUrl.hpp"
#include <com/sun/star/uri/XVndSunStarScriptUrlReference.hpp>
#include "com/sun/star/beans/XPropertySet.hpp"

using namespace ::com::sun::star;
using namespace ::com::sun::star::uno;
using namespace ::com::sun::star::frame;
using namespace ::com::sun::star::util;
using namespace ::com::sun::star::beans;
using namespace ::com::sun::star::lang;
using namespace ::com::sun::star::script;
using namespace ::com::sun::star::script::provider;
using namespace ::com::sun::star::document;

namespace scripting_protocolhandler
{

const sal_Char * const MYSERVICENAME = "com.sun.star.frame.ProtocolHandler";
const sal_Char * const MYIMPLNAME = "com.sun.star.comp.ScriptProtocolHandler";
const sal_Char * MYSCHEME = "vnd.sun.star.script";
const sal_Int32 MYSCHEME_LEN = 20;

rtl_StandardModuleCount g_moduleCount = MODULE_COUNT_INIT;

void SAL_CALL ScriptProtocolHandler::initialize( 
    const css::uno::Sequence < css::uno::Any >& aArguments )
    throw ( css::uno::Exception )
{
    if ( m_bInitialised )
    {
        return ;
    }

    // first argument contains a reference to the frame (may be empty or the desktop,
    // but usually it's a "real" frame)
    if ( aArguments.getLength() &&
         sal_False == ( aArguments[ 0 ] >>= m_xFrame ) )
    {
        ::rtl::OUString temp = OUSTR( "ScriptProtocolHandler::initialize: could not extract reference to the frame" );
        throw RuntimeException( temp, Reference< XInterface >() );
    }

    ENSURE_OR_THROW( m_xCtx.is(), "ScriptProtocolHandler::initialize: No Component Context available" );
    m_bInitialised = true;
}

Reference< XDispatch > SAL_CALL ScriptProtocolHandler::queryDispatch(
    const URL& aURL, const ::rtl::OUString& sTargetFrameName, sal_Int32 nSearchFlags )
    throw( ::com::sun::star::uno::RuntimeException )
{
	(void)sTargetFrameName;
	(void)nSearchFlags;

    Reference< XDispatch > xDispatcher;
    // get scheme of url

    Reference< uri::XUriReferenceFactory > xFac ( uri::UriReferenceFactory::create( m_xCtx ) );
    if ( xFac.is() )
    {
        Reference<  uri::XUriReference > uriRef(
            xFac->parse( aURL.Complete ), UNO_QUERY );
        if ( uriRef.is() )
        {
            if ( uriRef->getScheme().equals( ::rtl::OUString::createFromAscii( ::scripting_protocolhandler::MYSCHEME ) ) )
            {
                xDispatcher = this;
            }
        }
    }

    return xDispatcher;
}

Sequence< Reference< XDispatch > > SAL_CALL
ScriptProtocolHandler::queryDispatches(
const Sequence < DispatchDescriptor >& seqDescriptor )
throw( RuntimeException )
{
    sal_Int32 nCount = seqDescriptor.getLength();
    Sequence< Reference< XDispatch > > lDispatcher( nCount );
    for ( sal_Int32 i = 0; i < nCount; ++i )
    {
        lDispatcher[ i ] = this->queryDispatch( seqDescriptor[ i ].FeatureURL,
                                                seqDescriptor[ i ].FrameName,
                                                seqDescriptor[ i ].SearchFlags );
    }
    return lDispatcher;
}

void SAL_CALL ScriptProtocolHandler::dispatchWithNotification(
    const URL& aURL, const Sequence < PropertyValue >& lArgs,
    const Reference< XDispatchResultListener >& xListener )
    throw ( RuntimeException )
{

    sal_Bool bSuccess = sal_False;
    Any invokeResult;
	bool bCaughtException = sal_False;
	Any aException;
    Sequence< Any > inArgs( 0 );

    if ( m_bInitialised )
    {
        ::rtl::OUString aReferer;
        if ( lArgs.getLength() > 0 )
        {
            int argCount = 0;
            for ( int index = 0; index < lArgs.getLength(); index++ )
            {
                // The propertyval named "Referer"
                // is not an argument to be passed to script
                if ( lArgs[ index ].Name.compareToAscii("Referer") == 0 ) {
                    lArgs [ index ].Value >>= aReferer;
                } else {
                    inArgs.realloc( ++argCount );
                    inArgs[ argCount - 1 ] = lArgs[ index ].Value;
                }
            }
        }
        try
        {
            Reference< uri::XUriReferenceFactory > xFac( uri::UriReferenceFactory::create( m_xCtx ) );
            Reference< uri::XVndSunStarScriptUrlReference > xScriptUri( xFac->parse( aURL.Complete ), UNO_QUERY_THROW );
            ::rtl::OUString sLocation = xScriptUri->getParameter( ::rtl::OUString( RTL_CONSTASCII_USTRINGPARAM( "location" ) ) );
            bool bIsDocumentScript = ( sLocation == ::rtl::OUString( RTL_CONSTASCII_USTRINGPARAM( "document" ) ) );
            bool bRefererIsTrusted = ( aReferer.compareToAscii("private:", 8) == 0 );

            // obtain the component for our security check. We could check bIsDocumentScript but the "location" could be forged
            if ( getScriptInvocation() ) {
                    Reference< XEmbeddedScripts > xDocumentScripts;
                    xDocumentScripts.set( m_xScriptInvocation->getScriptContainer(), UNO_SET_THROW );

                OSL_ENSURE( xDocumentScripts.is(), "ScriptProtocolHandler::dispatchWithNotification: can't do the security check!" );
                if ( !xDocumentScripts.is() ||
                     ( !bRefererIsTrusted && !xDocumentScripts->getAllowMacroExecution() ) )
                {
                    if ( xListener.is() )
                    {
                        ::com::sun::star::frame::DispatchResultEvent aEvent(
                                static_cast< ::cppu::OWeakObject* >( this ), 
                                ::com::sun::star::frame::DispatchResultState::FAILURE, 
                                invokeResult );
                        try
                        {
                            xListener->dispatchFinished( aEvent ) ;
                        }
                        catch(RuntimeException & e)
                        {
                            OSL_TRACE( 
                                "ScriptProtocolHandler::dispatchWithNotification: caught RuntimeException"
                                "while dispatchFinished with failture of the execution %s",
                                ::rtl::OUStringToOString( e.Message,
                                RTL_TEXTENCODING_ASCII_US ).pData->buffer );
                        }
                    }
                    return;
                }
            }

            // Creates a ScriptProvider ( if one is not created already ) 
            createScriptProvider();

            Reference< provider::XScript > xFunc =
                m_xScriptProvider->getScript( aURL.Complete );
            ENSURE_OR_THROW( xFunc.is(),
                "ScriptProtocolHandler::dispatchWithNotification: validate xFunc - unable to obtain XScript interface" );

           
            Sequence< Any > outArgs( 0 );
            Sequence< sal_Int16 > outIndex;

            // attempt to protect the document against the script tampering with its Undo Context
            ::std::auto_ptr< ::framework::DocumentUndoGuard > pUndoGuard;
            if ( bIsDocumentScript )
                pUndoGuard.reset( new ::framework::DocumentUndoGuard( m_xScriptInvocation ) );

            bSuccess = sal_False;
            while ( !bSuccess )
            {
                Any aFirstCaughtException;
                try
                {
                    invokeResult = xFunc->invoke( inArgs, outIndex, outArgs );
                    bSuccess = sal_True;
                }
                catch( const provider::ScriptFrameworkErrorException& se )
                {
                    if  ( !aFirstCaughtException.hasValue() )
                        aFirstCaughtException = ::cppu::getCaughtException();

                    if ( se.errorType != provider::ScriptFrameworkErrorType::NO_SUCH_SCRIPT )
                        // the only condition which allows us to retry is if there is no method with the
                        // given name/signature
                        ::cppu::throwException( aFirstCaughtException );

                    if ( inArgs.getLength() == 0 )
                        // no chance to retry if we can't strip more in-args
                        ::cppu::throwException( aFirstCaughtException );

                    // strip one argument, then retry
                    inArgs.realloc( inArgs.getLength() - 1 );
                }
            }
        }
        // Office doesn't handle exceptions rethrown here very well, it cores,
        // all we can is log them and then set fail for the dispatch event!
        // (if there is a listener of course)
        catch ( const Exception & e )
        {
            aException = ::cppu::getCaughtException();

            ::rtl::OUString reason = ::rtl::OUString( RTL_CONSTASCII_USTRINGPARAM( "ScriptProtocolHandler::dispatch: caught " ) );

            invokeResult <<= reason.concat( aException.getValueTypeName() ).concat( e.Message );

			bCaughtException = sal_True;
        }
    }
    else
    {
        ::rtl::OUString reason = ::rtl::OUString::createFromAscii(
        "ScriptProtocolHandler::dispatchWithNotification failed, ScriptProtocolHandler not initialised"
        );
        invokeResult <<= reason;
    }

	if ( bCaughtException )
	{
		SfxAbstractDialogFactory* pFact = SfxAbstractDialogFactory::Create();

		if ( pFact != NULL )
		{
			VclAbstractDialog* pDlg =
				pFact->CreateScriptErrorDialog( NULL, aException );

			if ( pDlg != NULL )
			{
				pDlg->Execute();
				delete pDlg;
			}
		}
   	}

    if ( xListener.is() )
    {
        // always call dispatchFinished(), because we didn't load a document but
        // executed a macro instead!
        ::com::sun::star::frame::DispatchResultEvent aEvent;

        aEvent.Source = static_cast< ::cppu::OWeakObject* >( this );
        aEvent.Result = invokeResult;
        if ( bSuccess )
        {
            aEvent.State = ::com::sun::star::frame::DispatchResultState::SUCCESS;
        }
        else
        {
            aEvent.State = ::com::sun::star::frame::DispatchResultState::FAILURE;
        }

        try
        {
            xListener->dispatchFinished( aEvent ) ;
        }
        catch(RuntimeException & e)
        {
            OSL_TRACE( 
            "ScriptProtocolHandler::dispatchWithNotification: caught RuntimeException"
            "while dispatchFinished %s",
            ::rtl::OUStringToOString( e.Message,
            RTL_TEXTENCODING_ASCII_US ).pData->buffer );
        }
    }
}

void SAL_CALL ScriptProtocolHandler::dispatch(
const URL& aURL, const Sequence< PropertyValue >& lArgs )
throw ( RuntimeException )
{
    dispatchWithNotification( aURL, lArgs, Reference< XDispatchResultListener >() );
}

void SAL_CALL ScriptProtocolHandler::addStatusListener(
const Reference< XStatusListener >& xControl, const URL& aURL )
throw ( RuntimeException )
{
	(void)xControl;
	(void)aURL;

    // implement if status is supported
}

void SAL_CALL ScriptProtocolHandler::removeStatusListener(
const Reference< XStatusListener >& xControl, const URL& aURL )
throw ( RuntimeException )
{
	(void)xControl;
	(void)aURL;
}

bool
ScriptProtocolHandler::getScriptInvocation()
{
    if ( !m_xScriptInvocation.is() && m_xFrame.is() )
    {
        Reference< XController > xController = m_xFrame->getController();
        if ( xController .is() )
        {
            // try to obtain an XScriptInvocationContext interface, preferred from the
            // mode, then from the controller
            if ( !m_xScriptInvocation.set( xController->getModel(), UNO_QUERY ) )
                m_xScriptInvocation.set( xController, UNO_QUERY );
        }
        else
        {
            Reference< XFrame > xFrame( m_xFrame.get(), UNO_QUERY );
            if ( xFrame.is() )
            {
                SfxFrame* pFrame = NULL;
                for ( pFrame = SfxFrame::GetFirst(); pFrame; pFrame = SfxFrame::GetNext( *pFrame ) )
                {
                    if ( pFrame->GetFrameInterface() == xFrame )
                        break;
                }
                SfxObjectShell* pDocShell = pFrame ? pFrame->GetCurrentDocument() : SfxObjectShell::Current();
                if ( pDocShell )
                {
                    Reference< XModel > xModel( pDocShell->GetModel() );
                    m_xScriptInvocation.set( xModel, UNO_QUERY );
                }
            }
        }
    }
    return m_xScriptInvocation.is();
}

void ScriptProtocolHandler::createScriptProvider() 
{
    if ( m_xScriptProvider.is() )
        return;	

    try
    {
        // first, ask the component supporting the XScriptInvocationContext interface
        // (if there is one) for a script provider
        if ( getScriptInvocation() )
        {
            Reference< XScriptProviderSupplier > xSPS( m_xScriptInvocation, UNO_QUERY );
            if ( xSPS.is() )
                m_xScriptProvider = xSPS->getScriptProvider();
        }

        // second, ask the model in our frame
        if ( !m_xScriptProvider.is() && m_xFrame.is() )
        {
            Reference< XController > xController = m_xFrame->getController();
            if ( xController .is() )
            {
                Reference< XScriptProviderSupplier > xSPS( xController->getModel(), UNO_QUERY );
                if ( xSPS.is() )
                    m_xScriptProvider = xSPS->getScriptProvider();
            }
        }


        // as a fallback, ask the controller
        if ( !m_xScriptProvider.is() && m_xFrame.is() )
        {
            Reference< XScriptProviderSupplier > xSPS( m_xFrame->getController(), UNO_QUERY );
            if ( xSPS.is() )
                m_xScriptProvider = xSPS->getScriptProvider();
        }

        // if nothing of this is successful, use the master script provider
        if ( !m_xScriptProvider.is() )
        {
            ::rtl::OUString tmspf = ::rtl::OUString::createFromAscii(
                "/singletons/com.sun.star.script.provider.theMasterScriptProviderFactory");

            Reference< provider::XScriptProviderFactory > xFac(
                m_xCtx->getValueByName( tmspf ), UNO_QUERY_THROW );
                                                                                
            Any aContext;
            if ( getScriptInvocation() )
                aContext = makeAny( m_xScriptInvocation );
            m_xScriptProvider = Reference< provider::XScriptProvider > (
                xFac->createScriptProvider( aContext ), UNO_QUERY_THROW );
        }
    }
    catch ( RuntimeException & e )
    {
        ::rtl::OUString temp = OUSTR( "ScriptProtocolHandler::createScriptProvider(),  " );
        throw RuntimeException( temp.concat( e.Message ), Reference< XInterface >() );
    }
    catch ( Exception & e )
    {
        ::rtl::OUString temp = OUSTR( "ScriptProtocolHandler::createScriptProvider: " );
        throw RuntimeException( temp.concat( e.Message ), Reference< XInterface >() );
    }
}

ScriptProtocolHandler::ScriptProtocolHandler(
Reference< css::uno::XComponentContext > const& xCtx ) :
m_bInitialised( false ), m_xCtx( xCtx )
{
}

ScriptProtocolHandler::~ScriptProtocolHandler()
{
}

/* XServiceInfo */
::rtl::OUString SAL_CALL ScriptProtocolHandler::getImplementationName( )
throw( RuntimeException )
{
    return impl_getStaticImplementationName();
}

/* XServiceInfo */
sal_Bool SAL_CALL ScriptProtocolHandler::supportsService(
const ::rtl::OUString& sServiceName )
throw( RuntimeException )
{
    Sequence< ::rtl::OUString > seqServiceNames = getSupportedServiceNames();
    const ::rtl::OUString* pArray = seqServiceNames.getConstArray();
    for ( sal_Int32 nCounter = 0; nCounter < seqServiceNames.getLength(); nCounter++ )
    {
        if ( pArray[ nCounter ] == sServiceName )
        {
            return sal_True ;
        }
    }

    return sal_False ;
}

/* XServiceInfo */
Sequence< ::rtl::OUString > SAL_CALL ScriptProtocolHandler::getSupportedServiceNames()
throw( RuntimeException )
{
    return impl_getStaticSupportedServiceNames();
}

/* Helper for XServiceInfo */
Sequence< ::rtl::OUString > ScriptProtocolHandler::impl_getStaticSupportedServiceNames()
{
    ::osl::MutexGuard aGuard( ::osl::Mutex::getGlobalMutex() );
    Sequence< ::rtl::OUString > seqServiceNames( 1 );
    seqServiceNames.getArray() [ 0 ] =
        ::rtl::OUString::createFromAscii( ::scripting_protocolhandler::MYSERVICENAME );
    return seqServiceNames ;
}

/* Helper for XServiceInfo */
::rtl::OUString ScriptProtocolHandler::impl_getStaticImplementationName()
{
    return ::rtl::OUString::createFromAscii( ::scripting_protocolhandler::MYIMPLNAME );
}

/* Helper for registry */
Reference< XInterface > SAL_CALL ScriptProtocolHandler::impl_createInstance(
const Reference< css::uno::XComponentContext > & xCtx)
throw( RuntimeException )
{
    return Reference < XInterface >( *new ScriptProtocolHandler( xCtx ) );
}

static struct ::cppu::ImplementationEntry g_entries[] =
{
    {
        ScriptProtocolHandler::impl_createInstance,
        ScriptProtocolHandler::impl_getStaticImplementationName,
        ScriptProtocolHandler::impl_getStaticSupportedServiceNames,
        ::cppu::createSingleComponentFactory,
        &g_moduleCount.modCnt,
        0
    },
    { 0, 0, 0, 0, 0, 0 }
};

} // namespace scripting_protocolhandler

/* exported functions for registration */
extern "C"
{

#undef css
#define css ::com::sun::star
    
    void SAL_CALL component_getImplementationEnvironment( 
        const sal_Char** ppEnvironmentTypeName, uno_Environment** ppEnvironment )
    {
		(void)ppEnvironment;

        *ppEnvironmentTypeName = CPPU_CURRENT_LANGUAGE_BINDING_NAME ;
    }

    void* SAL_CALL component_getFactory( const sal_Char * pImplementationName ,
                                         void * pServiceManager ,
                                         void * pRegistryKey )
    {
        return component_getFactoryHelper( pImplementationName, pServiceManager, pRegistryKey, scripting_protocolhandler::g_entries );
    }
} // extern "C"


