# Configuration file for the Sphinx documentation builder.
#
# For the full list of built-in configuration values, see the documentation:
# https://www.sphinx-doc.org/en/master/usage/configuration.html
import os
import re

# Check if we're running on Read the Docs' servers
read_the_docs_build = os.environ.get('READTHEDOCS', None) == 'True'

# Links into the source tree. On Read the Docs, pin to the commit being built
# so every version, branch or PR build links to files that exist in it.
github_ref = os.environ.get('READTHEDOCS_GIT_COMMIT_HASH', 'v3.0')
github_blob = ('https://github.com/toyota-connected/ivi-homescreen/blob/'
               + github_ref)
extlinks = {'gh': (github_blob + '/%s', '%s')}

breathe_projects = {}

if read_the_docs_build:
    breathe_projects['ivi-homescreen'] = '../doxygen/xml'

# -- Project information -----------------------------------------------------
# https://www.sphinx-doc.org/en/master/usage/configuration.html#project-information

project = 'ivi-homescreen'
copyright = '2024, Joel Winarske'
author = 'Joel Winarske'
release = '"0.1"'

# -- General configuration ---------------------------------------------------
# https://www.sphinx-doc.org/en/master/usage/configuration.html#general-configuration

extensions = [
    'myst_parser',
    'sphinx_rtd_theme',
    'sphinx.ext.extlinks',
    'breathe'
]

source_suffix = {
    '.rst': 'restructuredtext',
    '.txt': 'markdown',
    '.md': 'markdown',
}

templates_path = ['_templates']
exclude_patterns = ['_build', 'Thumbs.db', '.DS_Store', 'requirements.txt', 'CMakeLists.txt']



# -- Options for HTML output -------------------------------------------------
# https://www.sphinx-doc.org/en/master/usage/configuration.html#options-for-html-output

html_theme = 'sphinx_rtd_theme'
html_static_path = ['../sphinx/_static']

# Breathe Configuration
breathe_default_project = "ivi-homescreen"

# readme.md is a symlink to the top-level README, whose links are relative to
# the repo root. Rewrite them to GitHub so they resolve here too.
_relative_link = re.compile(r'\]\((?!https?:|mailto:|#)([^)\s]+)\)')


def _rewrite_readme_links(app, docname, source):
    if docname == 'readme':
        source[0] = _relative_link.sub(
            lambda m: '](' + github_blob + '/' + m.group(1) + ')', source[0])


def setup(app):
    app.connect('source-read', _rewrite_readme_links)
