C_OBJS = $O\7zCrc.obj
all:
	IF "$(CPU)" == "IA64" || "$(CPU)" == "MIPS"
	C_OBJS = $(C_OBJS) \
	ELSE
	ASM_OBJS = $(ASM_OBJS) \
	ENDIF
	  $O\7zCrcOpt.obj
