@{
    # ---------------------------------------------------------------------------------
    #  TargetFacade: which of the vtable facade the COM server actually carries.
    #
    #  Read by check_api_drift.ps1. Upstream is TargetFacade.h, the pure-vtable public
    #  header; downstream is TargetCom.idl, the dual-interface server over it. Same shape
    #  as MsgFacade's config, and the naming here is even more mechanical: an interface
    #  X downstream is X plus "Com", and the event sink is a dispinterface with a leading
    #  underscore. TargetCom.idl writes that correspondence down in its own header
    #  comment; TypeMap transcribes it rather than parsing it, so the two can be found
    #  disagreeing instead of silently agreeing.
    #
    #  WHAT THIS PAIR IS FOR, in this repository specifically: a networking facade grows
    #  by accretion -- a new Send overload, a new diagnostic, a new event -- and each
    #  addition is one a scripting client cannot reach until somebody writes the IDL
    #  half. Nothing else notices. The vtable half compiles, the server compiles, the
    #  test that exercises the new method through C++ passes, and the Automation client
    #  goes on not having it.
    #
    #  ONE MAPPED-BUT-ABSENT ENTRY. IP2PDiagEvents is mapped to a dispinterface that does
    #  not exist. That is the point: the diagnostic sink has no Automation face today,
    #  and the allowlist is where that should be stated with a reason, not a config
    #  comment. If it gains one, the mapping is already correct and the entries clear
    #  themselves.
    #
    #  HubEventsBase and HubEventsBase2 are NOT mapped. They are convenience base structs
    #  with empty default implementations, provided so a C++ client can override one
    #  handler without writing the other nine. They are not a surface to bind -- there is
    #  nothing behind them -- and mapping them would report ten phantom gaps per class.
    # ---------------------------------------------------------------------------------

    AllowFile = 'tools/ci/api-drift.allow'

    Pairs = @(
        @{
            Id = 'facade-to-com'

            # TargetFacade.h only. The Fn and Topology headers are header-only helper
            # layers over these same interfaces: they bind nothing a COM client needs,
            # and one of them is a C++ template surface no IDL could express.
            Upstream = @{
                Kind  = 'cxx'
                Files = @('include/TargetFacade.h')
            }

            Surface = @{
                Kind  = 'idl'
                Files = @('com/TargetCom.idl')
            }

            TypeMap = @{
                'IP2PHub'        = 'IP2PHubCom'
                'IP2PMessage'    = 'IP2PMessageCom'
                'IP2PNetwork'    = 'IP2PNetworkCom'
                'IP2PHubEvents'  = '_IP2PHubEvents'
                'IP2PHubEvents2' = '_IP2PHubEvents'
                'IP2PDiagEvents' = '_IP2PDiagEvents'
            }
        }
    )
}
