﻿/********************************************************
 *                                                       *
 *   Copyright (C) Microsoft. All rights reserved.       *
 *                                                       *
 ********************************************************/

#include "pch.h"
#include "TerminalControl.h"
#include <sstream>
#include "../../renderer/inc/DummyRenderTarget.hpp"
#include "Win2DEngine.h"
#include <windows.ui.xaml.media.dxinterop.h>

using namespace winrt;
using namespace winrt::Windows::UI::Xaml;
using namespace winrt::Windows::UI::Xaml::Input;
using namespace winrt::Windows::UI::ViewManagement;

using namespace winrt::Microsoft::Graphics::Canvas;
using namespace winrt::Microsoft::Graphics::Canvas::Text;
using namespace winrt::Microsoft::Graphics::Canvas::UI::Xaml;

using namespace winrt::Windows::Foundation::Numerics;
using namespace winrt::Windows::Foundation;
using namespace winrt::Windows::System;

using namespace winrt::Windows::UI;
using namespace winrt::Windows::UI::Core;
using namespace winrt::Windows::ApplicationModel::Core;

using namespace ::Microsoft::Terminal::Core;
using namespace ::Microsoft::Console::Render;
using namespace ::Microsoft::Console::Types;

namespace winrt::TerminalComponent::implementation
{
    TerminalControl::TerminalControl() :
        // _connection{TerminalConnection::EchoConnection()},
        _connection{TerminalConnection::ConhostConnection(winrt::to_hstring("cmd.exe"), 30, 80)},
        // _connection{TerminalConnection::ConhostConnection(winrt::to_hstring("ssh.exe localhost"), 30, 80)},
        // _connection{ TerminalConnection::SshBackdoorConnection() },
        _initializedTerminal{ false }
    {
        InitializeComponent();

        //_canvasView = TerminalCanvasView( canvas00(), L"Consolas", 12.0f );
        //// Do this to avoid having to std::bind(canvasControl_Draw, this, placeholders::_1)
        //// Which I don't even know if it would work

        //canvas00().Draw([&](const auto& s, const auto& args) { terminalView_Draw(s, args); });
        //canvas00().CreateResources([&](const auto& /*s*/, const auto& /*args*/)
        //{
        //    _canvasView.Initialize();
        //    if (!_initializedTerminal)
        //    {
        //        // The Canvas view must be initialized first so we can get the size from it.
        //        _InitializeTerminal();
        //    }
        //});

        _InitializeTerminal();

        //// These are important:
        // 1. When we get tapped, focus us
        this->Tapped([&](auto&, auto& e) {
            Focus(FocusState::Pointer);
            e.Handled(true);
        });
        // 2. Focus us. (this might not be important
        this->Focus(FocusState::Programmatic);
        // 3. Make sure we can be focused (why this isn't `Focusable` I'll never know)
        this->IsTabStop(true);
        // 4. Actually not sure about this one. Maybe it isn't necessary either.
        this->AllowFocusOnInteraction(true);
    }

    void TerminalControl::_InitializeTerminal()
    {
        if (_initializedTerminal)
        {
            return;
        }

        // DO NOT USE canvase00().Width(), those are NaN for some reason
        /*float windowWidth = canvas00().ActualWidth();
        float windowHeight = canvas00().ActualHeight();*/
        float windowWidth = 300;
        float windowHeight = 300;
        /*COORD viewportSizeInChars = _canvasView.PixelsToChars(windowWidth, windowHeight);*/

        _terminal = new Terminal();

        // First create the render thread.
        auto renderThread = std::make_unique<RenderThread>();
        // Stash a local pointer to the render thread, so we can enable it after
        //       we hand off ownership to the renderer.
        auto* const localPointerToThread = renderThread.get();
        _renderer = std::make_unique<Renderer>(_terminal, nullptr, 0, std::move(renderThread));
        IRenderTarget& renderTarget = *_renderer;

        _terminal->Create({ 10, 10 }, 9001, renderTarget);

        auto dxEngine = std::make_unique<::Microsoft::Console::Render::DxEngine>();

        /*_renderEngine = std::make_unique<Win2DEngine>(_canvasView,
                                                      Viewport::FromDimensions({ 0, 0 }, { 10, 10 }));*/
        _renderer->AddRenderEngine(dxEngine.get());

        FontInfoDesired fi(L"Consolas", 0, 10, { 8, 12 }, 437);
        FontInfo actual(L"Consolas", 0, 10, { 8, 12 }, 437, false);
        _renderer->TriggerFontChange(96, fi, actual);

        THROW_IF_FAILED(dxEngine->SetWindowSize({ (LONG)windowWidth, (LONG)windowHeight }));
        dxEngine->SetCallback(std::bind(&TerminalControl::SwapChainChanged, this));
        THROW_IF_FAILED(dxEngine->Enable());
        _renderEngine = std::move(dxEngine);

        auto onRecieveOutputFn = [&](const hstring str) {
            _terminal->Write(str.c_str());
        };
        _connectionOutputEventToken = _connection.TerminalOutput(onRecieveOutputFn);
        _connection.Resize(10, 10);
        _connection.Start();

        auto inputFn = [&](const std::wstring& wstr)
        {
            _connection.WriteInput(wstr);
        };
        _terminal->_pfnWriteInput = inputFn;

        localPointerToThread->Initialize(_renderer.get());
        
        auto chain = _renderEngine->GetSwapChain();
        canvas00().Dispatcher().RunAsync(CoreDispatcherPriority::High, [=]()
        {
            _terminal->LockConsole();
            auto nativePanel = canvas00().as<ISwapChainPanelNative>();
            nativePanel->SetSwapChain(chain.Get());
            _terminal->UnlockConsole();
        });

        localPointerToThread->EnablePainting();

        // No matter what order these guys are in, The KeyDown's will fire
        //      before the CharacterRecieved, so we can't easily get characters
        //      first, then fallback to getting keys from vkeys.
        // TODO: This apparently handles keys and characters correctly, though
        //      I'd keep an eye on it, and test more.
        // I presume that the characters that aren't translated by terminalInput
        //      just end up getting ignored, and the rest of the input comes
        //      through CharacterRecieved.
        // I don't believe there's a difference between KeyDown and
        //      PreviewKeyDown for our purposes
        this->PreviewKeyDown([&](auto& sender, KeyRoutedEventArgs const& e){
            this->KeyHandler(sender, e);
        });

        this->CharacterReceived([&](auto& sender, CharacterReceivedRoutedEventArgs const& e) {
            this->CharacterHandler(sender, e);
        });

        this->canvas00().SizeChanged([&](auto& sender, SizeChangedEventArgs const& e) {

        });

        _initializedTerminal = true;
    }

    Terminal& TerminalControl::GetTerminal()
    {
        return *_terminal;
    }

    TerminalConnection::ITerminalConnection& TerminalControl::GetConnection()
    {
        return _connection;
    }

    void TerminalControl::Prototype_WriteToOutput(hstring const& text)
    {
        std::wstring str(text);
        _terminal->Write(str);
    }

    void TerminalControl::Prototype_ChangeTextColors(uint8_t fgIndex, uint8_t bgIndex)
    {
        _terminal->SetTextForegroundIndex(fgIndex);
        _terminal->SetTextBackgroundIndex(bgIndex);
    }

    void TerminalControl::SwapChainChanged()
    {
        auto chain = _renderEngine->GetSwapChain();
        canvas00().Dispatcher().RunAsync(CoreDispatcherPriority::High, [=]()
        {
            _terminal->LockConsole();
            auto nativePanel = canvas00().as<ISwapChainPanelNative>();
            nativePanel->SetSwapChain(chain.Get());
            _terminal->UnlockConsole();
        });
    }

    void TerminalControl::CharacterHandler(IInspectable const& /*sender*/,
                                           CharacterReceivedRoutedEventArgs const& e)
    {
        const auto ch = e.Character();
        if (ch == L'\x08')
        {
            // We want Backspace to be handled by KeyHandler, so the
            //      terminalInput can translate it into a \x7f. So, do nothing
            //      here, so we don't end up sending both a BS and a DEL to the
            //      terminal.
            return;
        }
        auto hstr = to_hstring(ch);
        _connection.WriteInput(hstr);
        e.Handled(true);
    }

    void TerminalControl::KeyHandler(IInspectable const& /*sender*/,
                                     KeyRoutedEventArgs const& e)
    {
        // This is super hacky - it seems as though these keys only seem pressed
        // every other time they're pressed
        CoreWindow foo = CoreWindow::GetForCurrentThread();
        // DONT USE
        //      != CoreVirtualKeyStates::None
        // OR
        //      == CoreVirtualKeyStates::Down
        // Sometimes with the key down, the state is Down | Locked.
        // Sometimes with the key up, the state is Locked.
        // IsFlagSet(Down) is the only correct solution.
        auto ctrlKeyState = foo.GetKeyState(winrt::Windows::System::VirtualKey::Control);
        auto shiftKeyState = foo.GetKeyState(winrt::Windows::System::VirtualKey::Shift);
        auto altKeyState = foo.GetKeyState(winrt::Windows::System::VirtualKey::Menu);

        auto ctrl = (ctrlKeyState & CoreVirtualKeyStates::Down) == CoreVirtualKeyStates::Down;
        auto shift = (shiftKeyState & CoreVirtualKeyStates::Down) == CoreVirtualKeyStates::Down;
        auto alt = (altKeyState & CoreVirtualKeyStates::Down) == CoreVirtualKeyStates::Down;

        auto vkey = e.OriginalKey();

        _terminal->SendKeyEvent((WORD)vkey, ctrl, alt, shift);
    }

    void TerminalControl::OnSizeChanged(IInspectable const& sender,
                                      SizeChangedEventArgs const& e)
    {
        const auto foundationSize = e.NewSize();
        SIZE classicSize;
        classicSize.cx = foundationSize.Width;
        classicSize.cy = foundationSize.Height;

        _renderEngine->SetWindowSize(classicSize);
        _renderer->TriggerRedrawAll();
        const auto vp = Viewport::FromInclusive(_renderEngine->GetDirtyRectInChars());
        _terminal->Resize({ vp.Width(), vp.Height() });
        _connection.Resize(vp.Height(), vp.Width());
    }
}
