# -*- coding: utf-8 -*-
#
# Sphinx configuration for the Trossen SDK documentation.
# Mirrors the Trossen Arm docs conf.py so look-and-feel stays consistent.

import time

project = 'Trossen Robotics Trossen SDK Documentation'
author = 'Trossen Robotics'
copyright = "{}, {}".format(time.strftime("%Y"), author)

extensions = [
    'sphinx_copybutton',
    'sphinx_tabs.tabs',
    'sphinx.ext.autosectionlabel',
    'sphinx.ext.extlinks',
    'sphinx.ext.githubpages',
    'sphinxcontrib.mermaid',
    'sphinxcontrib.youtube',
]

# Enable AutoSectionLabel
autosectionlabel_prefix_document = True

# sphinx_copybutton — strip "$ " prompts when copying shell blocks.
copybutton_prompt_text = '$ '

source_suffix = '.rst'
master_doc = 'index'
language = 'en'
exclude_patterns = ['_build', 'Thumbs.db', '.DS_Store']
pygments_style = 'sphinx'

# -- HTML output -------------------------------------------------------------

html_theme = 'sphinx_rtd_theme'
html_scaled_image_link = False
html_title = "Trossen Robotics Trossen SDK Documentation"

html_theme_options = {
    'collapse_navigation': True,
    'sticky_navigation': True,
    'navigation_depth': -1,
    'includehidden': True,
    'titles_only': False,
    'logo_only': True,
    'style_external_links': False,
}

html_logo = "images/logo.png"

html_static_path = ['_static']
html_css_files = ['tr_style.css']

html_favicon = './favicon.ico'

html_show_sourcelink = False
html_sourcelink_suffix = ''

html_context = {
    'display_github': True,
    'github_user': 'TrossenRobotics',
    'github_repo': 'trossen_sdk',
    'github_version': 'main/',
    'conf_py_path': 'docs/',
    'source_suffix': '.rst',
}

htmlhelp_basename = 'TrossenRoboticsTrossenSDKDocumentation'

primary_domain = 'cpp'
highlight_language = 'cpp'

# Make external links open in new tabs — same trick used by the Trossen Arm docs.
# https://stackoverflow.com/a/61669375

from sphinx.writers.html import HTMLTranslator
from docutils import nodes
from docutils.nodes import Element


class PatchedHTMLTranslator(HTMLTranslator):

    def visit_reference(self, node: Element) -> None:
        atts = {'class': 'reference'}
        if node.get('internal') or 'refuri' not in node:
            atts['class'] += ' internal'
        else:
            atts['class'] += ' external'
            atts['target'] = '_blank'
            atts['rel'] = 'noopener noreferrer'
        if 'refuri' in node:
            atts['href'] = node['refuri'] or '#'
            if self.settings.cloak_email_addresses and atts['href'].startswith('mailto:'):
                atts['href'] = self.cloak_mailto(atts['href'])
                self.in_mailto = True
        else:
            assert 'refid' in node, \
                'References must have "refuri" or "refid" attribute.'
            atts['href'] = '#' + node['refid']
        if not isinstance(node.parent, nodes.TextElement):
            assert len(node) == 1 and isinstance(node[0], nodes.image)
            atts['class'] += ' image-reference'
        if 'reftitle' in node:
            atts['title'] = node['reftitle']
        if 'target' in node:
            atts['target'] = node['target']
        self.body.append(self.starttag(node, 'a', '', **atts))

        if node.get('secnumber'):
            self.body.append(('%s' + self.secnumber_suffix) %
                             '.'.join(map(str, node['secnumber'])))


def setup(app):
    app.set_translator('html', PatchedHTMLTranslator, override=True)
