#pragma once

namespace NetUtil
{
void	  PrintError(int errorcode, int line);
long long MakeSessionUID(int sessionIdx, int sessionId);
} // namespace NetUtil
