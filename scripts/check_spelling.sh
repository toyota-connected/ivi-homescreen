#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 ivi-homescreen contributors
#
# check_spelling.sh — enforce US English in our own sources and docs.
#
# Usage:
#   scripts/check_spelling.sh
#
# The project writes US English. This catches the spellings that drift in and
# that review keeps missing. It is deliberately a list of whole words rather
# than a stem match, because stems produce false positives that get the whole
# check switched off -- "realis" hits "realistic", "emphasis" and "analysis"
# are correct US nouns, and most -ise verbs (advise, promise, exercise,
# franchise) are correct everywhere.
#
# Adding a word: put the complete non-US spelling in WORDS. Adding a genuine
# exception, such as an external API we have to spell their way: put the
# surrounding token in ALLOW.

set -euo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"

# Complete non-US spellings, matched case-insensitively on word boundaries.
WORDS=(
    colour colours coloured colouring colourful
    behaviour behaviours
    flavour flavours flavoured
    honour honours honoured neighbour neighbours
    favour favours favoured favourite favourites
    labour labours armour harbour odour rumour vapour
    centre centres centred centring
    metre metres millimetre millimetres kilometre kilometres
    litre litres fibre fibres calibre calibres
    theatre lustre sombre spectre
    licence licences defence defences offence offences pretence
    analyse analysed analyses analysing analyser
    paralyse paralysed
    practise practised practising
    catalogue catalogued programme programmes
    grey greyscale
    chequer chequered chequerboard
    normalise normalised normalises normalising normalisation
    optimise optimised optimises optimising optimisation optimisations
    organise organised organises organising organisation
    recognise recognised recognises recognising
    authorise authorised authorises authorising authorisation
    initialise initialised initialises initialising initialisation
    serialise serialised serialising serialisation
    deserialise deserialised deserialising
    synchronise synchronised synchronises synchronising synchronisation
    visualise visualised visualising visualisation
    finalise finalised finalising
    specialise specialised specialising
    standardise standardised standardising
    customise customised customising customisation
    prioritise prioritised prioritising
    utilise utilised utilising utilisation
    minimise minimised minimising
    maximise maximised maximising
    summarise summarised summarising
    categorise categorised categorising
    characterise characterised characterising
    sanitise sanitised sanitising
    randomise randomised randomising
    quantise quantised quantising
    virtualise virtualised localise localised localisation
    generalise generalised neutralise neutralised
    stabilise stabilised capitalise capitalised
    cancelled cancelling labelled labelling modelled modelling
    travelled travelling signalled signalling
    aluminium
)

# Tokens that legitimately carry a non-US spelling. Each is matched as a
# substring of the offending line; keep the reason with it.
ALLOW=(
    'unrecognised_options'  # cxxopts API: allow_unrecognised_options()
)

mapfile -d '' FILES < <(
    find \
        "${REPO_ROOT}/shell" \
        "${REPO_ROOT}/shared" \
        "${REPO_ROOT}/test" \
        "${REPO_ROOT}/scripts" \
        "${REPO_ROOT}/doc" \
        "${REPO_ROOT}/tools" \
        \( -name '*.cc' -o -name '*.cpp' -o -name '*.h' -o -name '*.hpp' \
           -o -name '*.md' -o -name '*.sh' -o -name '*.py' \
           -o -name 'CMakeLists.txt' -o -name '*.cmake' \) \
        -not -path "${REPO_ROOT}/shell/platform/homescreen/client_wrapper/*" \
        -not -path "${REPO_ROOT}/shell/platform/homescreen/public/*" \
        -not -path "${BASH_SOURCE[0]}" \
        -not -name "check_spelling.sh" \
        -print0 2>/dev/null
)

if [[ ${#FILES[@]} -eq 0 ]]; then
    echo "error: no files to check -- has the layout moved?" >&2
    exit 1
fi

PATTERN="\\b($(IFS='|'; echo "${WORDS[*]}"))\\b"

# -H so the filename is always printed: grep omits it for a single file, which
# would both hide the location and break the file:line parsing in ALLOW below.
HITS="$(grep -IHEnoi "${PATTERN}" "${FILES[@]}" || true)"

if [[ -n "${HITS}" ]]; then
    for allowed in "${ALLOW[@]}"; do
        # Drop lines whose file:line also contains an allowed token.
        HITS="$(
            while IFS= read -r hit; do
                [[ -z "${hit}" ]] && continue
                file="${hit%%:*}"
                rest="${hit#*:}"
                line="${rest%%:*}"
                if sed -n "${line}p" "${file}" 2>/dev/null | grep -qF "${allowed}"; then
                    continue
                fi
                printf '%s\n' "${hit}"
            done <<< "${HITS}"
        )"
    done
fi

if [[ -n "${HITS}" ]]; then
    echo "Not US English:" >&2
    printf '%s\n' "${HITS}" | sed "s|^${REPO_ROOT}/||" >&2
    echo >&2
    echo "Fix the spelling, or -- if an external API forces it -- add the" >&2
    echo "surrounding token to ALLOW in scripts/check_spelling.sh." >&2
    exit 1
fi

echo "US English: $(( ${#FILES[@]} )) file(s) clean."
