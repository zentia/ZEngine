#pragma once

namespace Z
{
    class GUIView
    {
    private:
        /* data */
    public:
        GUIView(/* args */);
        ~GUIView();
        static void RepaintAll(bool performAutorepaint);
    };

} // namespace Z