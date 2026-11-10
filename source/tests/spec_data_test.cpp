// Tests for the Specializations page reader. Run with the path of the saved page as the argument.
#include "spec_data.hpp"

#include <fstream>
#include <iostream>
#include <sstream>

namespace {
int failures = 0;
void Check(bool ok, const char* what) {
    if (!ok) {
        std::cerr << "FAIL: " << what << "\n";
        ++failures;
    }
}
}  // namespace

int main(int argc, char** argv) {
    using namespace esrwiki::spec;
    Data none;
    std::string error;
    Check(!Parse("<html>no board</html>", none, error), "a page without a board fails");

    const char* tiny =
        "<style>.spw { width: 600px; height: 700px; }</style>"
        "<table><tr><td>Intro <font color=\"orange\">Name</font> text<br>Second</td></tr></table>"
        "<div class=\"spw-wrap\"><div class=\"spw\">"
        "<input type=\"radio\" id=\"spw-tab1\" checked=\"checked\"><input type=\"radio\" id=\"spw-tab2\">"
        "<label for=\"spw-tab1\" class=\"spw-tab spw-tab1\" style=\"left:10px;\">One</label>"
        "<label for=\"spw-tab2\" class=\"spw-tab spw-tab2\" style=\"left:150px;\">Two</label>"
        "<div class=\"spw-title\">Title</div><div class=\"spw-points\">Hint</div>"
        "<div class=\"spw-page spw-page1\"><div class=\"spw-perk\" style=\"left:5px;top:6px;\">"
        "<div class=\"spw-icon spw-icon-alpha\" style=\"background-position:-132px 0;\"></div>"
        "<div class=\"spw-tip\"><div class=\"spw-tip-title\">P &amp; Q</div><div class=\"spw-tip-text\">Does a<br>thing</div>"
        "<div class=\"spw-tip-hint\">Hint<br>Cost</div></div></div></div>"
        "<div class=\"spw-page spw-page2\"></div></div></div>";
    Data d;
    Check(Parse(tiny, d, error), "tiny page parses");
    Check(d.tabs.size() == 2 && d.tabs[1].left == 150 && d.tabs[0].label == "One", "tabs");
    Check(d.width == 600 && d.height == 700, "board size from css");
    Check(d.title == "Title" && d.hint == "Hint", "title and hint");
    Check(d.intro.size() == 2 && d.intro[0].find("\xC3\xBF" "cOName") != std::string::npos, "intro with orange name");
    Check(d.tabs[0].perks.size() == 1 && d.tabs[0].perks[0].icon == 2 && d.tabs[0].sheet == "alpha", "perk icon and sheet");
    Check(d.tabs[0].perks[0].name == "P & Q" && d.tabs[0].perks[0].text == "Does a\nthing" && d.tabs[0].perks[0].hint == "Hint\nCost", "perk text");
    Check(d.tabs[0].perks[0].x == 5 && d.tabs[0].perks[0].y == 6, "perk position");

    if (argc > 1) {
        std::ifstream f(argv[1], std::ios::binary);
        std::stringstream ss;
        ss << f.rdbuf();
        Data page;
        Check(Parse(ss.str(), page, error), "website page parses");
        Check(page.tabs.size() == 4, "four specializations");
        for (const Tab& t : page.tabs) Check(t.perks.size() == 15, "fifteen perks per tab");
        Check(page.intro.size() >= 10, "intro text");
    }
    if (failures == 0) std::cout << "spec_data tests passed\n";
    return failures == 0 ? 0 : 1;
}
