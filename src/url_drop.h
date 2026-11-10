#pragma once
#include "common.h"
#include <oleidl.h>
#include <functional>
namespace jisu {
using UrlDropHandler=std::function<bool(POINTL,const std::wstring*,bool)>;
ComPtr<IDropTarget> MakeUrlDropTarget(UrlDropHandler handler);
}
