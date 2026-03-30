// =========================================================================
// == SUBSISTEMA DE MEMORIA Y REGISTROS DEL HARDWARE MIPS R3000A PSX =======
// =========================================================================

// Definición inamovible de las constantes vectoriales y máscaras del CP0
constexpr uint32_t VECTOR_INTERRUPCION_GENERAL = 0x80000080;
constexpr uint32_t CP0_MASCARA_INTERRUPCION_ACTUAL = 0x1;
constexpr uint32_t CP0_BIT_DELAY_SLOT = (1 << 31);

// Representación en memoria de los registros del Coprocesador de Control (CP0)
struct Coprocessor_0_State {
    uint32_t status;        // Registro $12: Jerarquía de máscaras y Estado global
    uint32_t cause;         // Registro $13: Taxonomía del origen de la excepción
    uint32_t epc;           // Registro $14: Ubicación de rescate / Retorno de fallo
    uint32_t bad_vaddr;     // Registro $8: Rastreo de accesos ilícitos de memoria
};

// La cápsula central de contexto. El JIT asume acceso directo (vía punteros estáticos
// o anclaje a un registro del procesador anfitrión) a esta estructura monolítica.
struct CPU_Environment {
    uint32_t pc;                    // Puntero de instrucción arquitectónico invitado
    uint32_t delay_slot_target_pc;  // Caché de latencia para destinos diferidos de saltos
    uint32_t gpr;               // 32 Registros escalares estándar de propósito general
    uint32_t hi, lo;                // Almacenamiento especial multiplicativo/divisorio
    
    Coprocessor_0_State cp0;        // Jerarquía incrustada del Coprocesador
    
    // Variables críticas de instrumentación y barreras de control de simulación
    int32_t downcounter;            // Margen cíclico asignado para ejecución ininterrumpida
    bool flag_irq_asincrona;        // Señal difusora (Lazy-poisoning) de evento no sincronizado
    bool flag_en_delay_slot;        // Detector de tránsito sobre la zona crítica de retardo
};

// =========================================================================
// == MOTOR DE PLANIFICACIÓN MATEMÁTICA Y TEMPORIZACIÓN DE EVENTOS (SCHEDULER)==
// =========================================================================

struct Simulated_Event {
    uint64_t sello_temporal_absoluto_ciclos; 
    void (*callback_ejecucion)(CPU_Environment*); // Enlace de función del dispositivo perimetral
};

class Chrono_Event_Scheduler {
private:
    uint64_t reloj_global_absoluto_sistema = 0;
    PriorityQueue<Simulated_Event> cola_de_eventos;

public:
    // Retorna la amplitud de la ventana temporal permitida antes de la próxima 
    // interrupción programada predecible, acotando el presupuesto (quantum) de la CPU.
    int32_t computar_delta_ventana_ejecucion() {
        if (cola_de_eventos.esta_vacia()) {
            return 10000; // Asignación conservadora predeterminada en el vacío
        }
        Simulated_Event proximo_evento = cola_de_eventos.observar_proximo();
        int32_t delta = proximo_evento.sello_temporal_absoluto_ciclos - reloj_global_absoluto_sistema;
        return (delta > 0)? delta : 0;
    }
    
    void avanzar_reloj_y_despachar(int32_t ciclos_efectivamente_consumidos, CPU_Environment* ctx) {
        reloj_global_absoluto_sistema += ciclos_efectivamente_consumidos;
        while (!cola_de_eventos.esta_vacia() && 
               cola_de_eventos.observar_proximo().sello_temporal_absoluto_ciclos <= reloj_global_absoluto_sistema) {
            Simulated_Event evento = cola_de_eventos.extraer();
            evento.callback_ejecucion(ctx); // Puede desencadenar interrupciones de hardware internamente
        }
    }
};

// =========================================================================
// == EVALUACIÓN PERIMETRAL: INYECCIÓN DE ABORTO "LAZY" (YIELDING) ======
// =========================================================================

// Esta función se invoca desde los callbacks del Scheduler (p.ej., el VBLANK del GPU 
// se ha completado, o el canal de DMA del SPU ha transferido el último bloque).
void evaluar_y_disparar_irq_hardware(CPU_Environment* ctx, uint32_t mascara_irq_periferica) {
    // 1. Alterar atómicamente la memoria mapeada del bus I_STAT simulado (0x1F801074)
    uint32_t estado_previo = Acceso_Memoria_Lenta::leer_palabra32(0x1F801074);
    Acceso_Memoria_Lenta::escribir_palabra32(0x1F801074, estado_previo | mascara_irq_periferica);
    
    // 2. Extraer validaciones duales (estado latente y permisos globales de la Placa Base)
    uint32_t registro_i_stat = Acceso_Memoria_Lenta::leer_palabra32(0x1F801074);
    uint32_t registro_i_mask = Acceso_Memoria_Lenta::leer_palabra32(0x1F801080);
    
    // Regla de decisión lógica fundamental: Permisos periféricos && Permisos Centrales MIPS
    if ((registro_i_stat & registro_i_mask)!= 0 && (ctx->cp0.status & CP0_MASCARA_INTERRUPCION_ACTUAL)) {
        
        // 3. INYECCIÓN DEL ABORTO DIFERIDO (LAZY POISONING)
        // Alterar destructivamente el cronómetro remanente fuerza al JIT 
        // a fracturar el enlace de bloques masivo en la próxima coyuntura válida.
        ctx->flag_irq_asincrona = true;
        ctx->downcounter = 0; 
    }
}

// Consumación litúrgica del Contexto MIPS una vez que el JIT ha capitulado y cedido la ejecución
void resolver_captura_excepcional(CPU_Environment* ctx) {
    if (!ctx->flag_irq_asincrona) return; // Retorno trivial en caso de expiración natural por límite de tiempo
    ctx->flag_irq_asincrona = false;
    
    // Complicación arquitectónica obligatoria: Protección y salvaguarda de Branch Delay Slots
    if (ctx->flag_en_delay_slot) {
        // Obligatorio: Retrotraer el PC y grabar la dirección de la sentencia de salto que causó la bifurcación
        ctx->cp0.epc = ctx->pc - 4;
        ctx->cp0.cause |= CP0_BIT_DELAY_SLOT;  // Empalmar el flag del Cause Register (Bit 31)
    } else {
        ctx->cp0.epc = ctx->pc;
        ctx->cp0.cause &= ~CP0_BIT_DELAY_SLOT; // Limpiar la bandera de anomalía condicional
    }
    
    // Insertar el código del motivo de interrupción exterior (0x00 para INT External Hardware)
    ctx->cp0.cause = (ctx->cp0.cause & 0xFFFFFF83) | 0x0; 
    
    // Traslación escalonada de las máscaras de estado de interrupción dentro de los bits [5:0]
    uint32_t sub_modo_profundo = ctx->cp0.status & 0x3F;
    ctx->cp0.status = (ctx->cp0.status & ~0x3F) | ((sub_modo_profundo << 2) & 0x3F);
    
    // Obligar a la redirección de flujo global hacia la fortaleza del kernel en el mapa de memoria
    ctx->pc = VECTOR_INTERRUPCION_GENERAL;
    ctx->delay_slot_target_pc = VECTOR_INTERRUPCION_GENERAL + 4;
}

// =========================================================================
// == SÍNTESIS DE LA RECOMPILACIÓN Y ENLACE ESTRUCTURAL DE BLOQUES (JIT)  ==
// =========================================================================

struct Analitica_Bloque_JIT {
    uint32_t id_pc_origen;
    void (*codigo_ejecutable_nativo_x86_arm)(CPU_Environment*); 
    bool validacion_cache_activa;
    Analitica_Bloque_JIT* puntero_enlace_directo_saltado;    // Cadena estática A -> B (Take)
    Analitica_Bloque_JIT* puntero_enlace_directo_consecutivo; // Cadena estática A -> C (No Take)
};

Analitica_Bloque_JIT* compilar_secuencia_bloque(uint32_t pc_inicial) {
    Analitica_Bloque_JIT* bloque = asignar_memoria_rastreable(pc_inicial);
    Intermediate_Representation_Builder emisor_ir;
    uint32_t pc_corriente = pc_inicial;
    int suma_coste_ciclos = 0;
    bool terminador_bloque_alcanzado = false;

    while (!terminador_bloque_alcanzado) {
        uint32_t patron_hex_op = Acceso_Memoria_Rapida::leer_memoria_principal(pc_corriente);
        Instruccion_RISC instr_mips = decodificador_mips::interpretar(patron_hex_op);
        suma_coste_ciclos++;

        if (instr_mips.altera_flujo_de_control()) { // Evalúa JR, JAL, BEQ, BLEZ, SYSCALL...
            terminador_bloque_alcanzado = true;
            
            // EL INFIERNO DEL "BRANCH DELAY SLOT": REORDENACIÓN EN TIEMPO DE COMPILACIÓN
            
            // Paso 1. Avanzar artificialmente la mirada en la memoria, leyendo la subsecuente
            // instrucción física sin que la lógica del compilador anfitrión avance su puntero
            uint32_t patron_delay_slot = Acceso_Memoria_Rapida::leer_memoria_principal(pc_corriente + 4);
            Instruccion_RISC instr_sombra_ds = decodificador_mips::interpretar(patron_delay_slot);
            
            // Salvaguarda: El estándar del procesador R3000 aborrece las ramas consecutivas en slots superpuestos
            if (instr_sombra_ds.altera_flujo_de_control()) {
                // Caer intencionadamente en penalización: Transfiriendo ejecución temporal
                // al bucle interpretativo secuencial puro ante comportamientos caóticos
                emisor_ir.emitir_parada_fallback_interprete(); 
                break;
            }
            
            // Paso 2. Re-alineación Lógica: El compilador escribe primero la emisión semántica 
            // de la instrucción cautiva, incrustándola en la matriz anfitriona ANTES de las rutinas de salto
            emisor_ir.inyectar_estado_bandera("flag_en_delay_slot", true);
            emisor_ir.emitir_maquinaria_escalar(instr_sombra_ds);
            emisor_ir.inyectar_estado_bandera("flag_en_delay_slot", false);
            suma_coste_ciclos++;
            
            // Paso 3. Recién en este instante, se incrustan en el backend las operaciones atómicas
            // correspondientes a las comparaciones booleanas matemáticas requeridas por el salto original
            emisor_ir.emitir_veredicto_ramificacion_condicional(instr_mips); 
            
        } else {
            // Emisión de ALU tradicional, sin interrupciones espaciotemporales (Add, Sub, Lw, Sw, Sll...)
            emisor_ir.emitir_maquinaria_escalar(instr_mips);
            pc_corriente += 4;
        }
    }

    // ARQUITECTURA DEL EPÍLOGO DEL BLOQUE JIT: INTERSECCIÓN DEL PLANIFICADOR
    // Aquí se genera el código nativo (ensamblador del PC físico) para sustracción temporal
    emisor_ir.emitir_resta_variable_memoria("downcounter", suma_coste_ciclos);
    
    // Inyección de la frontera vital del "Block Linking". Equivale en pseudocódigo máquina a:
    // CMP [rcx+offset_downcounter], 0 
    // JLE.abort_yield_exit
    // JMP [rcx+offset_puntero_enlace] <-- Esta línea será brutalmente sobrescrita 
    //                                     (Parcheo directo) al compilar el siguiente eslabón.
    emisor_ir.emitir_rutina_seguridad_ciclica_o_rendicion();

    bloque->codigo_ejecutable_nativo_x86_arm = emisor_ir.sintetizar_y_bloquear_memoria_rx();
    bloque->validacion_cache_activa = true;
    return bloque;
}

// =========================================================================
// == DESPACHADOR CENTRAL: EL MOTOR INFINITO (DISPATCH LOOP) ================
// =========================================================================

void iniciar_secuencia_arranque_consola(CPU_Environment* ctx, Chrono_Event_Scheduler* planificador) {
    while (true) { // Bucle sempiterno de alimentación energética de la placa madre simulada
        
        // Fase 1: Recabar autorizaciones temporales mediante la tasación de los intervalos periféricos
        int32_t ciclos_concedidos_cuantum = planificador->computar_delta_ventana_ejecucion();
        ctx->downcounter = ciclos_concedidos_cuantum;
        
        // Fase 2: Hundimiento en las trincheras aceleradas de código nativo anfitrión (El Pozo JIT)
        while (ctx->downcounter > 0) {
            
            // Resolución indirecta. Localizar por hash si el MIPS intenta invocar rutas pre-exploradas
            Analitica_Bloque_JIT* bloque_inminente = tabla_hash_directorio_bloques::buscar(ctx->pc);
            
            if (!bloque_inminente) {
                // Falla en caché térmica: Costosa invocación del compilador On-The-Fly.
                bloque_inminente = compilar_secuencia_bloque(ctx->pc);
                
                // Enlace A posteriori: El despachador busca en los rincones de los epílogos pasados,
                // reparando los punteros ciegos para tejer hilos de ejecución ininterrumpidos en el futuro
                optimizador_parches::sellar_enlace_bloque_previo_con_nuevo(bloque_inminente); 
            }
            
            // PUNTO SINGULAR DE TRANSFERENCIA
            // Un salto a través de puntero de función hacia el ensamblador puro del bloque.
            // Una vez que la ejecución entre aquí, el despachador perderá consciencia de sí mismo 
            // hasta que un bloque agote el "downcounter" natural o forzosamente y decida rendirse (Yield).
            bloque_inminente->codigo_ejecutable_nativo_x86_arm(ctx);
            
            // [Cese de Inmersión]
        }

        // Fase 3: Expiación cronológica y reparación perimetral.
        // Al regresar a la consciencia central, es imperativo conciliar los registros bancarios de tiempo
        int32_t ciclos_efectivamente_calcinados = ciclos_concedidos_cuantum - ctx->downcounter;
        
        // El universo circundante (GPU, CD-ROM) reacciona y evoluciona en la misma proporción de tiempo transcurrido
        planificador->avanzar_reloj_y_despachar(ciclos_efectivamente_calcinados, ctx);

        // Fase 4: La Inquisición del Coprocesador. Verificar si el retorno prematuro 
        // a este bucle se debió a una señal de socorro asíncrona externa dictaminada por 'Lazy Poisoning'.
        if (ctx->flag_irq_asincrona) {
            resolver_captura_excepcional(ctx);
        }
    }
}