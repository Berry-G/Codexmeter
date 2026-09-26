#pragma once
#include <d2d1.h>
#include <dwrite.h>
#ifdef small
#undef small
#endif
#pragma comment(lib,"d2d1.lib")
#pragma comment(lib,"dwrite.lib")
#include "quota.hpp"

class StripRenderer {
    ID2D1Factory* factory = nullptr;
    IDWriteFactory* textFactory = nullptr;
    ID2D1DCRenderTarget* surface = nullptr;
    ID2D1SolidColorBrush* brush = nullptr;
    IDWriteTextFormat* caption = nullptr;
    IDWriteTextFormat* number = nullptr;
    IDWriteTextFormat* time = nullptr;
    template<class T> static void release(T*& p) { if (p) p->Release(); p = nullptr; }
    static D2D1_COLOR_F color(unsigned hex,float alpha = 1.0f) {
        return D2D1::ColorF(((hex>>16)&255)/255.0f,((hex>>8)&255)/255.0f,(hex&255)/255.0f,alpha);
    }
    bool initialize() {
        if (!factory && FAILED(D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED,&factory))) return false;
        if (!textFactory && FAILED(DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED,__uuidof(IDWriteFactory),reinterpret_cast<IUnknown**>(&textFactory)))) return false;
        auto createFormat = [&](IDWriteTextFormat*& format,float size,DWRITE_FONT_WEIGHT weight) {
            if (format) return true;
            if (FAILED(textFactory->CreateTextFormat(L"Segoe UI",nullptr,weight,DWRITE_FONT_STYLE_NORMAL,DWRITE_FONT_STRETCH_NORMAL,size,L"ko-KR",&format))) return false;
            format->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
            format->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
            return true;
        };
        if (!createFormat(caption,10.5f,DWRITE_FONT_WEIGHT_NORMAL) || !createFormat(number,11.5f,DWRITE_FONT_WEIGHT_SEMI_BOLD) || !createFormat(time,11.0f,DWRITE_FONT_WEIGHT_NORMAL)) return false;
        if (!surface) {
            auto props = D2D1::RenderTargetProperties(D2D1_RENDER_TARGET_TYPE_SOFTWARE,
                D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM,D2D1_ALPHA_MODE_PREMULTIPLIED));
            if (FAILED(factory->CreateDCRenderTarget(&props,&surface))) return false;
            surface->SetTextAntialiasMode(D2D1_TEXT_ANTIALIAS_MODE_GRAYSCALE);
            if (FAILED(surface->CreateSolidColorBrush(color(0),&brush))) { release(surface); return false; }
        }
        return true;
    }
    void text(const std::wstring& value,IDWriteTextFormat* format,D2D1_RECT_F rect,unsigned ink) {
        brush->SetColor(color(ink));
        surface->DrawTextW(value.c_str(),static_cast<UINT32>(value.size()),format,rect,brush,D2D1_DRAW_TEXT_OPTIONS_CLIP);
    }
public:
    ~StripRenderer() { release(brush); release(surface); release(caption); release(number); release(time); release(textFactory); release(factory); }
    bool paint(HDC dc,RECT pixels,unsigned dpi,const meter::Quota& q,bool stale,const std::wstring& status,const std::wstring& reset,bool dark) {
        if (!initialize() || FAILED(surface->BindDC(dc,&pixels))) return false;
        surface->SetDpi(static_cast<float>(dpi),static_cast<float>(dpi));
        surface->BeginDraw(); surface->Clear(D2D1::ColorF(0,0.0f));
        float width = pixels.right*96.0f/dpi;
        unsigned ink = dark ? 0xe9edf5 : 0x252c38;
        unsigned muted = dark ? 0xa3adbd : 0x747c89;
        unsigned track = dark ? 0x454e5d : 0xcbd1da;
        int count = static_cast<int>(q.shortTerm.known)+static_cast<int>(q.weekly.known);
        if (!count) text(status.empty()?L"한도 정보 없음":status,caption,D2D1::RectF(6,0,width-6,24),muted);
        else {
            bool showReset = width >= (count == 2 ? 260 : 180);
            float resetWidth = showReset ? 88.0f : 0.0f;
            float barWidth = (width-12-(count-1)*10-resetWidth-(showReset?10:0))/count;
            float x = 6;
            auto bar = [&](const wchar_t* title,const meter::Window& w,unsigned accent) {
                text(title,caption,D2D1::RectF(x,0,x+29,18),muted);
                text(std::to_wstring(w.remaining)+L"%"+(stale?L"*":L""),number,D2D1::RectF(x+31,0,x+barWidth,18),ink);
                brush->SetColor(color(track));
                surface->FillRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(x,20,x+barWidth,22),1,1),brush);
                if (w.remaining > 0) {
                    unsigned tint = stale ? muted : w.remaining <= 10 ? (dark?0xf08b86:0xc4524c) : w.remaining <= 25 ? (dark?0xe3b86a:0xb77f24) : accent;
                    brush->SetColor(color(tint));
                    surface->FillRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(x,20,x+barWidth*w.remaining/100.0f,22),1,1),brush);
                }
                x += barWidth+10;
            };
            if (q.shortTerm.known) bar(L"단기",q.shortTerm,dark?0x66c4b2:0x348e7f);
            if (q.weekly.known) bar(L"주간",q.weekly,dark?0x80a9ee:0x507fd0);
            if (showReset) {
                brush->SetColor(color(muted,0.28f));
                surface->DrawLine(D2D1::Point2F(x-5,5),D2D1::Point2F(x-5,17),brush,0.8f);
                brush->SetColor(color(muted));
                surface->DrawEllipse(D2D1::Ellipse(D2D1::Point2F(x+4,11),3.2f,3.2f),brush,0.9f);
                surface->DrawLine(D2D1::Point2F(x+4,8.8f),D2D1::Point2F(x+4,11),brush,0.9f);
                surface->DrawLine(D2D1::Point2F(x+4,11),D2D1::Point2F(x+5.7f,12),brush,0.9f);
                text(status.empty()?reset:stale?L"이전 정보":L"연결 중",time,D2D1::RectF(x+13,0,width-6,22),muted);
            }
        }
        HRESULT result = surface->EndDraw();
        if (result == D2DERR_RECREATE_TARGET) { release(brush); release(surface); }
        return SUCCEEDED(result);
    }
};
